import assert from "node:assert/strict";
import type { CodexBankedResetConfig } from "../src/codex_banked_reset.ts";
import type { CodexUsageResetProvider } from "../src/codex_banked_reset_provider.ts";
import type { CodexAuthPoolState } from "../src/types.ts";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { setStreamFirstEventDeadlineMsForTest } from "../src/inference_deadline.ts";
import { RELEASE_GIT_SHA } from "../src/release.ts";
import { sha256Base64Url, sha256Hex } from "../src/utils.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
const TEST_CODEX_MODELS_KEY = ["ubq_ai", "codex_models"] as const;

const kvStore = new Map<string, unknown>();
type OpenAiAtomicOp = { type: "set" | "delete"; key: Deno.KvKey; value?: unknown };
let atomicCommitFailure: ((ops: readonly OpenAiAtomicOp[]) => Error | null) | null = null;
let exposePaidFallbackLedgerEntries = false;
kvStore.set(keyToString(DEFAULT_REASONING_EFFORT_KEY), "low");
kvStore.set(keyToString(["ubq_ai", "codex_auth"]), {
  accounts: [{
    access_token: "access",
    refresh_token: "refresh",
    account_id: "acct",
    updated_at_ms: Date.now(),
  }],
  updated_at_ms: Date.now(),
});
kvStore.set(keyToString(TEST_CODEX_MODELS_KEY), {
  source: "chatgpt_codex",
  client_version: "0.125.0",
  updated_at_ms: Date.now(),
  models: [{
    slug: DEFAULT_TEST_MODEL,
    display_name: "GPT-5 Fixture Default",
    context_window: 272000,
    max_context_window: 1000000,
    auto_compact_token_limit: null,
    default_reasoning_level: "medium",
    supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh", "max", "ultra"],
    reasoning_effort_wire_map: { ultra: "max" },
  }],
});
kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage_test_key");

const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
  set: (key: Deno.KvKey, value: unknown) => {
    kvStore.set(keyToString(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* (selector: Deno.KvListSelector, _options?: Deno.KvListOptions) {
    if (!exposePaidFallbackLedgerEntries || !("prefix" in selector)) return;
    for (const [encoded, value] of kvStore) {
      const key = JSON.parse(encoded) as Deno.KvKey;
      if (!selector.prefix.every((part, index) => key[index] === part)) continue;
      yield { key, value, versionstamp: "00000000000000000001" } as Deno.KvEntry<unknown>;
    }
  },
  atomic: () => {
    const ops: OpenAiAtomicOp[] = [];
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
        const failure = atomicCommitFailure?.(ops) ?? null;
        if (failure) return Promise.reject(failure);
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
  extractUsageTokens,
  getResponseTelemetry,
  handleChatCompletions,
  handleModelCapabilities,
  handleModels,
  handleResponses,
  setCodexBankedResetOptionsForTest,
} = await import("../src/openai.ts");
const { withCors } = await import("../src/http.ts");
const { resetRuntimeConfigCacheForTest } = await import("../src/runtime_config.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
const { attemptCodexBankedReset } = await import("../src/codex_banked_reset.ts");
const {
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  getCodexQuotaBlockFence,
  isCodexQuotaBlockFenceCurrent,
  markCodexQuotaBlocked,
  selectCodexRoutingAccounts,
} = await import("../src/codex_account_routing.ts");

const TEXT_ENCODER = new TextEncoder();

class Deferred<T> {
  readonly promise: Promise<T>;
  #resolve!: (value: T | PromiseLike<T>) => void;

  constructor() {
    this.promise = new Promise<T>((resolve) => {
      this.#resolve = resolve;
    });
  }

  resolve(value: T): void {
    this.#resolve(value);
  }
}

const encodeBase64 = (bytes: Uint8Array): string => {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
};

const encodeBase64Url = (bytes: Uint8Array): string =>
  encodeBase64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const encodeJsonBase64Url = (value: unknown): string => encodeBase64Url(TEXT_ENCODER.encode(JSON.stringify(value)));

const toPublicKeyPem = (spki: Uint8Array): string => {
  const b64 = encodeBase64(spki);
  const lines = b64.match(/.{1,64}/g) ?? [];
  return `-----BEGIN PUBLIC KEY-----\n${lines.join("\n")}\n-----END PUBLIC KEY-----`;
};

const sseResponse = (chunks: string[]): Response => {
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(TEXT_ENCODER.encode(chunk));
      controller.close();
    },
  });
  return new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } });
};

const baseSseChunks = () => [
  `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_test", created_at: 0 } })}\n\n`,
  `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
  `data: ${
    JSON.stringify({
      type: "response.completed",
      response: {
        model: DEFAULT_TEST_MODEL,
        output: [],
        usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
      },
    })
  }\n\n`,
];

const seedPaidFallbackKey = (
  id: string,
  options: {
    enabled?: boolean;
    limitMicrocredits?: number;
    spentMicrocredits?: number;
    reservedMicrocredits?: number;
    reservationRequestId?: string | null;
    modelIds?: readonly string[];
    v3SettledMicrocredits?: number;
  } = {},
): void => {
  const now = Date.now();
  const windowMs = 60_000;
  const windowResetAtMs = now + windowMs;
  const limitMicrocredits = options.limitMicrocredits ?? 1_000_000;
  const pricingCheckedAtMs = now;
  const hash = `hash-${id}`;
  const common = {
    paid_fallback_enabled: options.enabled ?? true,
    paid_fallback_limit_microcredits: limitMicrocredits,
    paid_fallback_spent_microcredits: options.spentMicrocredits ?? 0,
    paid_fallback_reserved_microcredits: options.reservedMicrocredits ?? 0,
    paid_fallback_reservation_request_id: options.reservationRequestId ?? null,
  };
  kvStore.set(keyToString(["ubq_ai", "api_keys", "id", id]), {
    id,
    name: `Key ${id}`,
    prefix: "u_test",
    hash,
    created_at_ms: now,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: windowResetAtMs,
    window_ms: windowMs,
    ...common,
    paid_fallback_model_ids: options.modelIds ?? [DEFAULT_TEST_MODEL],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { [DEFAULT_TEST_MODEL]: 250_000 },
    paid_fallback_pricing_checked_at_ms: pricingCheckedAtMs,
  });
  kvStore.set(keyToString(["ubq_ai", "api_keys", "hash", hash]), {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: windowResetAtMs,
    window_ms: windowMs,
    ...common,
  });
  if (options.v3SettledMicrocredits !== undefined) {
    kvStore.set(keyToString(["uos_ai", "paid_fallback", "v3", "window", id, windowResetAtMs]), {
      v: 3,
      key_id: id,
      policy_version: `${windowMs}:${pricingCheckedAtMs}`,
      window_reset_at_ms: windowResetAtMs,
      limit_microcredits: limitMicrocredits,
      settled_microcredits: options.v3SettledMicrocredits,
      reserved_microcredits: 0,
      pending_count: 0,
      updated_at_ms: now,
    });
  }
};

type StoredPaidFallbackRequest = {
  dispatch_state?: string;
  terminal_state?: string;
  billing_state?: string;
  provider_request_id?: string | null;
};

const getStoredPaidFallbackRequest = (keyId: string, requestId: string): StoredPaidFallbackRequest | null =>
  (kvStore.get(
    keyToString(["uos_ai", "paid_fallback", "v3", "request", keyId, requestId]),
  ) as StoredPaidFallbackRequest | undefined) ?? null;

const waitForPaidFallbackTerminal = async (
  keyId: string,
  requestId: string,
  expected: string,
): Promise<StoredPaidFallbackRequest> => {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const request = getStoredPaidFallbackRequest(keyId, requestId);
    if (request?.terminal_state === expected) return request;
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  const request = getStoredPaidFallbackRequest(keyId, requestId);
  assert.fail(
    `Expected ${keyId}/${requestId} terminal_state=${expected}, received ${request?.terminal_state ?? "missing"}`,
  );
};

const parseWarnings = (value: string | null): string[] =>
  value ? value.split(",").map((entry) => entry.trim()).filter(Boolean) : [];

const extractResponseOutputText = (payload: Record<string, unknown>): string => {
  const output = payload.output;
  if (!Array.isArray(output)) return "";
  const chunks: string[] = [];
  for (const item of output) {
    if (!item || typeof item !== "object") continue;
    const content = (item as { content?: unknown }).content;
    if (!Array.isArray(content)) continue;
    for (const contentItem of content) {
      if (!contentItem || typeof contentItem !== "object") continue;
      const part = contentItem as { type?: unknown; text?: unknown };
      if (part.type === "output_text" && typeof part.text === "string") chunks.push(part.text);
    }
  }
  return chunks.join("");
};

type FetchMockQueue = {
  chain: Promise<void>;
};

const fetchMockQueue: FetchMockQueue = (() => {
  const key = "__uosFetchMockQueue";
  const globalRecord = globalThis as unknown as Record<string, unknown>;
  const existing = globalRecord[key];
  if (existing && typeof existing === "object") {
    const chain = (existing as { chain?: unknown }).chain;
    if (chain instanceof Promise) return existing as FetchMockQueue;
  }
  const created: FetchMockQueue = { chain: Promise.resolve() };
  globalRecord[key] = created;
  return created;
})();

const withFetchMock = async <T>(
  handler: (url: string, bodyText: string | null, init?: RequestInit) => Response | Promise<Response>,
  fn: () => Promise<T>,
): Promise<T> => {
  const prev = fetchMockQueue.chain;
  let release = () => {};
  fetchMockQueue.chain = new Promise<void>((resolve) => {
    release = () => resolve(undefined);
  });
  await prev;

  const snapshot = kvStore.get(keyToString(TEST_CODEX_MODELS_KEY)) as
    | { models?: Array<Record<string, unknown>>; source?: string; updated_at_ms?: number; client_version?: string }
    | undefined;
  if (snapshot?.models?.length) {
    const explicitDefault = kvStore.get(keyToString(DEFAULT_MODEL_KEY));
    kvStore.set(keyToString(["uos_ai", "runtime_config", "v2"]), {
      version: 2,
      default_model: typeof explicitDefault === "string"
        ? explicitDefault
        : String(snapshot.models[0]?.slug ?? DEFAULT_TEST_MODEL),
      default_reasoning_effort: String(kvStore.get(keyToString(DEFAULT_REASONING_EFFORT_KEY)) ?? "low"),
      codex_models: snapshot,
      updated_at_ms: Date.now(),
    });
  } else {
    kvStore.delete(keyToString(["uos_ai", "runtime_config", "v2"]));
  }
  resetRuntimeConfigCacheForTest();
  // Each mocked exchange is an independent gateway isolate/request fixture.
  // Circuit behavior itself is covered by codex-account-routing.test.ts.
  kvStore.delete(keyToString(["uos_ai", "codex_account_routing", "v2"]));
  resetCodexAuthCacheForTest();

  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const bodyText = typeof init?.body === "string" ? init.body : null;
    return await handler(url, bodyText, init);
  };
  try {
    return await fn();
  } finally {
    globalThis.fetch = originalFetch;
    release();
  }
};

const clearBankedResetRecords = (): void => {
  for (const encodedKey of [...kvStore.keys()]) {
    const key = JSON.parse(encodedKey) as unknown[];
    if (key[0] === "uos_ai" && key[1] === "codex_reset_redemption") kvStore.delete(encodedKey);
  }
};

const liveBankedResetFixtureConfig = (accountId: string): CodexBankedResetConfig => ({
  enabled: true,
  mode: "live",
  accountAllowlist: new Set([accountId]),
  maxGlobalPerDay: 1,
  maxPerAccountPerWindow: 1,
});

const createVerifiedBankedResetFixture = async (): Promise<readonly string[]> => {
  const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
  const now = Date.now();
  const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, now);
  if (selection.kind !== "eligible") throw new Error(`Expected an eligible fixture account, got ${selection.kind}.`);
  const routing = selection.accounts[0]!;
  const blocked = await markCodexQuotaBlocked(
    routing,
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": new Date(now + 60_000).toUTCString() },
    }),
    now,
  );
  if (!blocked.usageLimitReached || blocked.retryAtMs === null) {
    throw new Error("Expected a durable usage-limit quota fence.");
  }
  const routingGeneration = await getCodexQuotaBlockFence(routing, blocked.retryAtMs);
  if (routingGeneration === null) throw new Error("Expected the durable quota fence to be readable.");

  const calls: string[] = [];
  const provider: CodexUsageResetProvider = {
    contract: {
      idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
      lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
      verification: { independentlyVerifiable: true },
      receiptIdsSafeToPersistAndLog: true,
      supportedResetTypes: ["codex_usage_limit"],
    },
    readInventory: () => {
      calls.push("inventory");
      return Promise.resolve({ availableCount: 1, observedAtMs: now, resetType: "codex_usage_limit" });
    },
    redeem: () => {
      calls.push("redeem");
      return Promise.resolve({ kind: "completed", providerReceiptId: "fixture-receipt" } as const);
    },
    lookup: () => {
      calls.push("lookup");
      return Promise.resolve({ kind: "completed", providerReceiptId: "fixture-receipt" } as const);
    },
    verifyApplied: () => {
      calls.push("verify");
      return Promise.resolve(true);
    },
  };
  const reset = await attemptCodexBankedReset(
    {
      accountId: routing.auth.account_id,
      credentialVersion: routing.credentialVersion,
      quotaResetAtMs: blocked.retryAtMs,
      routingGeneration,
      fences: [{
        key: CODEX_ACCOUNT_ROUTING_KV_KEY,
        isCurrent: (value) => isCodexQuotaBlockFenceCurrent(value, routing, blocked.retryAtMs!, routingGeneration),
      }],
      requestId: "openai-compat-verified-reset-fixture",
    },
    {
      config: liveBankedResetFixtureConfig(routing.auth.account_id),
      provider,
      kv: kvStub,
      now: () => now,
      newOwnerToken: () => "openai-compat-verified-reset-owner",
    },
  );
  assert.equal(reset.kind, "verified");
  assert.equal(reset.record?.routing_generation, routingGeneration);
  return calls;
};

/**
 * This creates a durable ambiguous transaction using only an in-memory fake.
 * The public handlers deliberately keep the shipped provider unavailable, so
 * their recovery-only path must return the ordinary quota error without ever
 * committing a successful response stream.
 */
const createUnknownBankedResetFixture = async (): Promise<readonly string[]> => {
  const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
  const now = Date.now();
  const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, now);
  if (selection.kind !== "eligible") throw new Error(`Expected an eligible fixture account, got ${selection.kind}.`);
  const routing = selection.accounts[0]!;
  const blocked = await markCodexQuotaBlocked(
    routing,
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": new Date(now + 60_000).toUTCString() },
    }),
    now,
  );
  if (!blocked.usageLimitReached || blocked.retryAtMs === null) {
    throw new Error("Expected a durable usage-limit quota fence.");
  }
  const routingGeneration = await getCodexQuotaBlockFence(routing, blocked.retryAtMs);
  if (routingGeneration === null) throw new Error("Expected the durable quota fence to be readable.");

  const calls: string[] = [];
  const provider: CodexUsageResetProvider = {
    contract: {
      idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
      lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
      verification: { independentlyVerifiable: true },
      receiptIdsSafeToPersistAndLog: true,
      supportedResetTypes: ["codex_usage_limit"],
    },
    readInventory: () => {
      calls.push("inventory");
      return Promise.resolve({ availableCount: 1, observedAtMs: now, resetType: "codex_usage_limit" });
    },
    redeem: () => {
      calls.push("redeem");
      return Promise.resolve({ kind: "unknown", providerReceiptId: null } as const);
    },
    lookup: () => {
      calls.push("lookup");
      return Promise.resolve({ kind: "unknown", providerReceiptId: null } as const);
    },
    verifyApplied: () => {
      calls.push("verify");
      return Promise.resolve(false);
    },
  };
  const reset = await attemptCodexBankedReset(
    {
      accountId: routing.auth.account_id,
      credentialVersion: routing.credentialVersion,
      quotaResetAtMs: blocked.retryAtMs,
      routingGeneration,
      fences: [{
        key: CODEX_ACCOUNT_ROUTING_KV_KEY,
        isCurrent: (value) => isCodexQuotaBlockFenceCurrent(value, routing, blocked.retryAtMs!, routingGeneration),
      }],
      requestId: "openai-compat-unknown-reset-fixture",
    },
    {
      config: liveBankedResetFixtureConfig(routing.auth.account_id),
      provider,
      kv: kvStub,
      now: () => now,
      newOwnerToken: () => "openai-compat-unknown-reset-owner",
    },
  );
  assert.equal(reset.kind, "pending");
  assert.equal(reset.record?.state, "unknown");
  return calls;
};

Deno.test("openai: verified banked reset recovers the fenced account before Responses delivery", async (t) => {
  for (const clientWantsStream of [false, true]) {
    await t.step(clientWantsStream ? "streamed" : "buffered", async () => {
      const delivery = clientWantsStream ? "streamed" : "buffered";
      const postResetText = `post-reset-${delivery}`;
      const upstreamUrls: string[] = [];
      const result = await withFetchMock(
        (url, bodyText) => {
          upstreamUrls.push(url);
          assert.ok(bodyText);
          const upstreamBody = JSON.parse(bodyText) as Record<string, unknown>;
          assert.equal(upstreamBody.stream, true, "Codex transport must remain SSE-shaped for both client modes.");
          return sseResponse([
            `data: ${
              JSON.stringify({ type: "response.created", response: { id: `resp_${delivery}`, created_at: 0 } })
            }\n\n`,
            `data: ${JSON.stringify({ type: "response.output_text.delta", delta: postResetText })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                },
              })
            }\n\n`,
          ]);
        },
        async () => {
          clearBankedResetRecords();
          try {
            const resetProviderCalls = await createVerifiedBankedResetFixture();
            const response = await handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  input: "recover an already verified reset",
                  ...(clientWantsStream ? { stream: true } : {}),
                }),
              }),
            );
            const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
            const routingAfterRecovery = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
            return { response, resetProviderCalls, routingAfterRecovery: routingAfterRecovery.kind };
          } finally {
            clearBankedResetRecords();
          }
        },
      );

      // The only mocked transport is the one permitted post-reset inference.
      // The banked-reset fake is in-memory, and recovery cannot call the
      // shipped unavailable provider when it finds the verified record.
      assert.deepEqual(upstreamUrls, ["https://chatgpt.com/backend-api/codex/responses"]);
      assert.deepEqual(result.resetProviderCalls, ["inventory", "redeem", "verify"]);
      assert.equal(result.routingAfterRecovery, "eligible", "the verified decision must clear its exact quota fence");
      assert.equal(result.response.status, 200);
      assert.equal(result.response.headers.get("x-uos-upstream"), "chatgpt_codex");
      if (clientWantsStream) {
        assert.equal(result.response.headers.get("Content-Type"), "text/event-stream");
        const stream = await result.response.text();
        assert.match(stream, new RegExp(postResetText));
        assert.match(stream, /response\.completed/);
      } else {
        const payload = await result.response.json() as Record<string, unknown>;
        assert.equal(extractResponseOutputText(payload), postResetText);
      }
    });
  }
});

Deno.test("openai: verified banked reset recovers the fenced account before Chat delivery", async (t) => {
  for (const clientWantsStream of [false, true]) {
    await t.step(clientWantsStream ? "streamed" : "buffered", async () => {
      const delivery = clientWantsStream ? "streamed" : "buffered";
      const postResetText = `chat-post-reset-${delivery}`;
      const upstreamUrls: string[] = [];
      const result = await withFetchMock(
        (url, bodyText) => {
          upstreamUrls.push(url);
          assert.ok(bodyText);
          return sseResponse([
            `data: ${
              JSON.stringify({ type: "response.created", response: { id: `chat_${delivery}`, created_at: 0 } })
            }\n\n`,
            `data: ${JSON.stringify({ type: "response.output_text.delta", delta: postResetText })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                },
              })
            }\n\n`,
          ]);
        },
        async () => {
          clearBankedResetRecords();
          try {
            const resetProviderCalls = await createVerifiedBankedResetFixture();
            const response = await handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  messages: [{ role: "user", content: "recover an already verified reset" }],
                  ...(clientWantsStream ? { stream: true } : {}),
                }),
              }),
            );
            return { response, resetProviderCalls };
          } finally {
            clearBankedResetRecords();
          }
        },
      );

      assert.deepEqual(upstreamUrls, ["https://chatgpt.com/backend-api/codex/responses"]);
      assert.deepEqual(result.resetProviderCalls, ["inventory", "redeem", "verify"]);
      assert.equal(result.response.status, 200);
      if (clientWantsStream) {
        assert.equal(result.response.headers.get("Content-Type"), "text/event-stream");
        const stream = await result.response.text();
        assert.match(stream, new RegExp(postResetText));
        assert.match(stream, /data: \[DONE\]/);
      } else {
        const payload = await result.response.json() as { choices?: Array<{ message?: { content?: unknown } }> };
        assert.equal(payload.choices?.[0]?.message?.content, postResetText);
      }
    });
  }
});

Deno.test("openai: an unknown banked reset returns an ordinary error with no successful stream bytes", async (t) => {
  const routes = [
    {
      name: "Responses",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            input: "unknown reset",
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleResponses,
    },
    {
      name: "Chat",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{ role: "user", content: "unknown reset" }],
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleChatCompletions,
    },
  ] as const;

  for (const route of routes) {
    for (const stream of [false, true]) {
      await t.step(`${route.name} ${stream ? "streamed" : "buffered"}`, async () => {
        const result = await withFetchMock(
          () => {
            throw new Error("An unknown reset must not dispatch a post-reset inference request.");
          },
          async () => {
            clearBankedResetRecords();
            try {
              const resetProviderCalls = await createUnknownBankedResetFixture();
              const response = await route.handle(route.request(stream));
              return { response, resetProviderCalls };
            } finally {
              clearBankedResetRecords();
            }
          },
        );

        assert.deepEqual(result.resetProviderCalls, ["inventory", "redeem"]);
        assert.equal(result.response.status, 429);
        assert.doesNotMatch(result.response.headers.get("Content-Type") ?? "", /text\/event-stream/i);
        const body = await result.response.text();
        assert.doesNotMatch(body, /(?:^|\n)data:\s|response\.(?:created|output_text\.delta|completed)/);
        const payload = JSON.parse(body) as { error?: { code?: unknown } };
        assert.equal(payload.error?.code, "codex_quota_blocked");
      });
    }
  }
});

Deno.test("openai: a post-reset 429 is returned once without a successful stream", async (t) => {
  const routes = [
    {
      name: "Responses",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            input: "post-reset 429",
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleResponses,
    },
    {
      name: "Chat",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{ role: "user", content: "post-reset 429" }],
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleChatCompletions,
    },
  ] as const;

  for (const route of routes) {
    for (const stream of [false, true]) {
      await t.step(`${route.name} ${stream ? "streamed" : "buffered"}`, async () => {
        let upstreamCalls = 0;
        const result = await withFetchMock(
          () => {
            upstreamCalls += 1;
            return new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
              status: 429,
              headers: {
                "Content-Type": "application/json",
                "Retry-After": new Date(Date.now() + 60_000).toUTCString(),
              },
            });
          },
          async () => {
            clearBankedResetRecords();
            try {
              const resetProviderCalls = await createVerifiedBankedResetFixture();
              const response = await route.handle(route.request(stream));
              return { response, resetProviderCalls };
            } finally {
              clearBankedResetRecords();
            }
          },
        );

        assert.equal(upstreamCalls, 1);
        assert.deepEqual(result.resetProviderCalls, ["inventory", "redeem", "verify"]);
        assert.equal(result.response.status, 429);
        assert.doesNotMatch(result.response.headers.get("Content-Type") ?? "", /text\/event-stream/i);
        const body = await result.response.text();
        assert.doesNotMatch(body, /(?:^|\n)data:\s|response\.(?:created|output_text\.delta|completed)/);
      });
    }
  }
});

Deno.test("openai: public handlers wait for verified banked redemption before one retry and delivery", async (t) => {
  const routes = [
    {
      name: "Responses",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            input: "qualifying reset flow",
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleResponses,
      read: async (response: Response, stream: boolean, text: string): Promise<void> => {
        if (stream) {
          assert.equal(response.headers.get("Content-Type"), "text/event-stream");
          assert.match(await response.text(), new RegExp(text));
          return;
        }
        assert.equal(extractResponseOutputText(await response.json() as Record<string, unknown>), text);
      },
    },
    {
      name: "Chat",
      request: (stream: boolean) =>
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{ role: "user", content: "qualifying reset flow" }],
            ...(stream ? { stream: true } : {}),
          }),
        }),
      handle: handleChatCompletions,
      read: async (response: Response, stream: boolean, text: string): Promise<void> => {
        if (stream) {
          assert.equal(response.headers.get("Content-Type"), "text/event-stream");
          const body = await response.text();
          assert.match(body, new RegExp(text));
          assert.match(body, /data: \[DONE\]/);
          return;
        }
        const payload = await response.json() as { choices?: Array<{ message?: { content?: unknown } }> };
        assert.equal(payload.choices?.[0]?.message?.content, text);
      },
    },
  ] as const;

  for (const route of routes) {
    for (const stream of [false, true]) {
      await t.step(`${route.name} ${stream ? "streamed" : "buffered"}`, async () => {
        const verificationGate = new Deferred<void>();
        const verificationEntered = new Deferred<void>();
        const providerCalls: string[] = [];
        const provider: CodexUsageResetProvider = {
          contract: {
            idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
            lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
            verification: { independentlyVerifiable: true },
            receiptIdsSafeToPersistAndLog: false,
            supportedResetTypes: ["codex_usage_limit"],
          },
          readInventory: () => {
            providerCalls.push("inventory");
            return Promise.resolve({
              availableCount: 1,
              observedAtMs: Date.now(),
              resetType: "codex_usage_limit",
            });
          },
          redeem: () => {
            providerCalls.push("redeem");
            return Promise.resolve({ kind: "completed", providerReceiptId: "endpoint-fixture-receipt" } as const);
          },
          lookup: () => {
            providerCalls.push("lookup");
            return Promise.resolve({ kind: "completed", providerReceiptId: "endpoint-fixture-receipt" } as const);
          },
          verifyApplied: async () => {
            providerCalls.push("verify");
            verificationEntered.resolve(undefined);
            await verificationGate.promise;
            return true;
          },
        };
        const upstreamUrls: string[] = [];
        const postResetText = `${route.name}-${stream ? "stream" : "buffer"}-verified`;
        const result = await withFetchMock(
          (url) => {
            upstreamUrls.push(url);
            if (upstreamUrls.length === 1) {
              return new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
                status: 429,
                headers: {
                  "Content-Type": "application/json",
                  "Retry-After": new Date(Date.now() + 60_000).toUTCString(),
                },
              });
            }
            if (upstreamUrls.length === 2) {
              return sseResponse([
                `data: ${
                  JSON.stringify({ type: "response.created", response: { id: "post_reset", created_at: 0 } })
                }\n\n`,
                `data: ${JSON.stringify({ type: "response.output_text.delta", delta: postResetText })}\n\n`,
                `data: ${
                  JSON.stringify({
                    type: "response.completed",
                    response: {
                      model: DEFAULT_TEST_MODEL,
                      output: [],
                      usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                    },
                  })
                }\n\n`,
              ]);
            }
            throw new Error("A verified banked reset may retry inference only once.");
          },
          async () => {
            clearBankedResetRecords();
            setCodexBankedResetOptionsForTest({
              config: liveBankedResetFixtureConfig("acct"),
              provider,
              kv: kvStub,
              now: () => Date.now(),
              newOwnerToken: () => `endpoint-${route.name}-${stream ? "stream" : "buffer"}`,
            });
            try {
              let responseResolved = false;
              const responsePromise = route.handle(route.request(stream)).then((response) => {
                responseResolved = true;
                return response;
              });
              await verificationEntered.promise;
              await Promise.resolve();
              assert.equal(responseResolved, false, "no public response may be committed before verification");
              assert.deepEqual(upstreamUrls, ["https://chatgpt.com/backend-api/codex/responses"]);
              assert.deepEqual(providerCalls, ["inventory", "redeem", "verify"]);
              verificationGate.resolve(undefined);
              return await responsePromise;
            } finally {
              setCodexBankedResetOptionsForTest(null);
              clearBankedResetRecords();
            }
          },
        );

        assert.deepEqual(upstreamUrls, [
          "https://chatgpt.com/backend-api/codex/responses",
          "https://chatgpt.com/backend-api/codex/responses",
        ]);
        assert.deepEqual(providerCalls, ["inventory", "redeem", "verify"]);
        assert.equal(result.status, 200);
        await route.read(result, stream, postResetText);
      });
    }
  }
});

Deno.test("openai: defaults + ignore temperature", async (t) => {
  await t.step("chat uses default model/reasoning and ignores temperature", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              temperature: 0.2,
              max_tokens: 12,
              moderation: { model: "omni-moderation-latest" },
              prompt_cache_options: { mode: "implicit", ttl: "30m" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { model?: string };
    assert.equal(payload.model, DEFAULT_TEST_MODEL);
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("temperature_ignored"));
    assert.ok(warnings.includes("max_output_tokens_ignored"));
    assert.ok(warnings.includes("moderation_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.deepEqual(recorded["prompt_cache_options"], { mode: "implicit", ttl: "30m" });
  });

  await t.step("chat preserves none reasoning effort upstream", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: "none",
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "none" });
  });

  await t.step("chat accepts null reasoning effort as unspecified", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: null,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
  });

  await t.step("responses uses default model/reasoning and ignores temperature", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              temperature: 0.7,
              max_output_tokens: 24,
              moderation: { model: "omni-moderation-latest" },
              prompt_cache_options: { mode: "implicit", ttl: "30m" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as Record<string, unknown> & { model?: string; reasoning?: unknown };
    assert.equal(payload.model, DEFAULT_TEST_MODEL);
    assert.equal(extractResponseOutputText(payload), "pong");
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("temperature_ignored"));
    assert.ok(warnings.includes("max_output_tokens_ignored"));
    assert.ok(warnings.includes("moderation_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.deepEqual(recorded["prompt_cache_options"], { mode: "implicit", ttl: "30m" });
  });

  await t.step("responses accepts and strips Codex CLI client metadata", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              input: "ping",
              client_metadata: {
                session_id: "session_test",
                thread_id: "thread_test",
                request_kind: "turn",
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.deepEqual(parseWarnings(response.headers.get("x-uos-warning")), []);
    assert.ok(recordedBody);
    assert.equal("client_metadata" in recordedBody, false);
  });

  await t.step("responses rejects malformed Codex CLI client metadata", async () => {
    const response = await handleResponses(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          model: DEFAULT_TEST_MODEL,
          input: "ping",
          client_metadata: { session_id: 123 },
        }),
      }),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { param?: string } };
    assert.equal(payload.error?.param, "client_metadata");
  });

  await t.step("responses rejects array-valued Codex CLI client metadata", async () => {
    const response = await handleResponses(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          model: DEFAULT_TEST_MODEL,
          input: "ping",
          client_metadata: ["session_test"],
        }),
      }),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { param?: string } };
    assert.equal(payload.error?.param, "client_metadata");
  });

  await t.step("responses preserves none reasoning upstream", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              reasoning: { effort: "none" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "none" });
  });

  await t.step("responses accepts null reasoning as unspecified", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              reasoning: null,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
  });

  await t.step("responses accepts null reasoning fields as unspecified", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              reasoning: {
                effort: null,
                summary: null,
                generate_summary: null,
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
  });

  await t.step("responses accepts official context_management parameter", async () => {
    let recordedBody: Record<string, unknown> | null = null;
    const contextManagement = [{ type: "compaction", compact_threshold: 2000 }];

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "ping",
              context_management: contextManagement,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.deepEqual(recorded["context_management"], contextManagement);
  });
});

Deno.test("openai: default model requires configured model or stored snapshot", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultModelKey = keyToString(DEFAULT_MODEL_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefault = kvStore.get(defaultModelKey);
  kvStore.delete(snapshotKey);
  kvStore.delete(defaultModelKey);

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("no-model requests should not fetch upstream defaults");
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ input: "ping" }),
          }),
        ),
    );

    assert.equal(response.status, 503);
    const payload = await response.json() as { error?: { message?: string; code?: string } };
    assert.equal(payload.error?.code, "server_error");
    assert.match(payload.error?.message ?? "", /no configured default model or Codex model snapshot/);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefault === undefined) kvStore.delete(defaultModelKey);
    else kvStore.set(defaultModelKey, previousDefault);
  }
});

Deno.test("openai: configured default reasoning survives missing catalog metadata", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultModelKey = keyToString(DEFAULT_MODEL_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefault = kvStore.get(defaultModelKey);
  const modelWithoutReasoningMetadata = "gpt-5-no-reasoning-metadata";
  kvStore.set(snapshotKey, {
    source: "codex_cli",
    client_version: "0.126.0",
    updated_at_ms: Date.now(),
    models: [{ slug: modelWithoutReasoningMetadata, display_name: "No Reasoning Metadata" }],
  });
  kvStore.delete(defaultModelKey);

  let recordedBody: Record<string, unknown> | null = null;
  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse([
          `data: ${
            JSON.stringify({ type: "response.created", response: { id: "resp_no_reasoning", created_at: 0 } })
          }\n\n`,
          `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                model: modelWithoutReasoningMetadata,
                output: [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            })
          }\n\n`,
        ]);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ input: "ping" }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    assert.equal((recordedBody as Record<string, unknown>).model, modelWithoutReasoningMetadata);
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "low" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefault === undefined) kvStore.delete(defaultModelKey);
    else kvStore.set(defaultModelKey, previousDefault);
  }
});

Deno.test("openai: default reasoning level is accepted when supported levels are absent", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const modelWithDefaultOnly = "gpt-5-default-reasoning-only";
  kvStore.set(snapshotKey, {
    source: "codex_cli",
    client_version: "0.126.0",
    updated_at_ms: Date.now(),
    models: [{
      slug: modelWithDefaultOnly,
      display_name: "Default Reasoning Only",
      default_reasoning_level: "medium",
    }],
  });

  let recordedBody: Record<string, unknown> | null = null;
  try {
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse([
          `data: ${
            JSON.stringify({ type: "response.created", response: { id: "resp_default_only", created_at: 0 } })
          }\n\n`,
          `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "pong" })}\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                model: modelWithDefaultOnly,
                output: [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            })
          }\n\n`,
        ]);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: modelWithDefaultOnly,
              input: "ping",
              reasoning: { effort: "medium" },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "medium" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: none remains a gateway special case when snapshot levels omit it", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);

  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      display_name: "GPT-5 Fixture Default",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
    }],
  });

  try {
    const capabilitiesResponse = await withFetchMock(
      () => {
        throw new Error("model capability reads should not fetch upstream");
      },
      () => handleModelCapabilities(),
    );
    assert.equal(capabilitiesResponse.status, 200);
    const capabilitiesPayload = await capabilitiesResponse.json() as {
      data?: Array<{ supported_reasoning_levels?: string[] }>;
    };
    assert.deepEqual(capabilitiesPayload.data?.[0]?.supported_reasoning_levels, [
      "none",
      "low",
      "medium",
      "high",
      "xhigh",
    ]);

    let recordedBody: Record<string, unknown> | null = null;
    const chatResponse = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: "none",
            }),
          }),
        ),
    );

    assert.equal(chatResponse.status, 200);
    assert.ok(recordedBody);
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "none" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: hostile catalog wire maps cannot rewrite none reasoning", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const defaultReasoningKey = keyToString(DEFAULT_REASONING_EFFORT_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousDefaultReasoning = kvStore.get(defaultReasoningKey);
  kvStore.set(defaultReasoningKey, "none");
  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      display_name: "Hostile wire-map fixture",
      default_reasoning_level: "none",
      supported_reasoning_levels: ["none", "max"],
      reasoning_effort_wire_map: { none: "max" },
    }],
  });

  try {
    const recordedEfforts: unknown[] = [];
    await withFetchMock(
      (_url, bodyText) => {
        const body = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : {};
        recordedEfforts.push((body.reasoning as Record<string, unknown> | undefined)?.effort);
        return sseResponse(baseSseChunks());
      },
      async () => {
        const chat = await handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ messages: [{ role: "user", content: "ping" }] }),
          }),
        );
        const responses = await handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ input: "ping", reasoning: { effort: "none" } }),
          }),
        );
        assert.equal(chat.status, 200);
        assert.equal(responses.status, 200);
      },
    );
    assert.deepEqual(recordedEfforts, ["none", "none"]);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousDefaultReasoning === undefined) kvStore.delete(defaultReasoningKey);
    else kvStore.set(defaultReasoningKey, previousDefaultReasoning);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: models returns stored Codex snapshot without upstream fetch", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("handleModels should not fetch upstream models");
    },
    () => handleModels(),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { data?: Array<Record<string, unknown> & { id?: string }> };
  assert.ok(Array.isArray(payload.data));
  const model = payload.data.find((entry) => entry.id === DEFAULT_TEST_MODEL);
  assert.ok(model);
  assert.deepEqual(Object.keys(model).sort(), ["created", "id", "object", "owned_by"]);
  assert.equal(model.object, "model");
  assert.equal(typeof model.created, "number");
  assert.equal(Object.prototype.hasOwnProperty.call(model, "supported_reasoning_levels"), false);
  assert.equal(Object.prototype.hasOwnProperty.call(model, "display_name"), false);
});

Deno.test("openai: models exposes API-supported hidden review models from the snapshot", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    client_version: "0.125.0",
    updated_at_ms: Date.now(),
    models: [{ slug: DEFAULT_TEST_MODEL }, {
      slug: "codex-auto-review",
      display_name: "Codex Auto Review",
      visibility: "hide",
      supported_in_api: true,
    }],
  });

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("handleModels should not fetch upstream models");
      },
      () => handleModels(),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { data?: Array<{ id?: string }> };
    assert.ok(payload.data?.some((model) => model.id === "codex-auto-review"));
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: model capabilities are exposed outside /v1 model objects", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("handleModelCapabilities should not fetch upstream models");
    },
    () => handleModelCapabilities(),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as {
    object?: string;
    data?: Array<{
      id?: string;
      object?: string;
      upstream_provider?: string;
      supported_endpoints?: string[];
      supported_reasoning_levels?: string[];
      default_reasoning_effort?: string | null;
      reasoning_effort_wire_map?: Record<string, string>;
      context_window_tokens?: number | null;
      max_context_window_tokens?: number | null;
      auto_compact_token_limit_tokens?: number | null;
    }>;
  };
  assert.equal(payload.object, "list");
  assert.ok(Array.isArray(payload.data));
  const model = payload.data.find((entry) => entry.id === DEFAULT_TEST_MODEL);
  assert.ok(model);
  assert.equal(model.object, "uos.model_capabilities");
  assert.equal(model.upstream_provider, "codex_chatgpt");
  assert.deepEqual(model.supported_reasoning_levels, ["none", "low", "medium", "high", "xhigh", "max", "ultra"]);
  assert.equal(model.default_reasoning_effort, "medium");
  assert.deepEqual(model.reasoning_effort_wire_map, { ultra: "max" });
  assert.equal(model.context_window_tokens, 272000);
  assert.equal(model.max_context_window_tokens, 1000000);
  assert.equal(model.auto_compact_token_limit_tokens, null);
  assert.ok(model.supported_endpoints?.includes("/v1/chat/completions"));
  assert.ok(model.supported_endpoints?.includes("/v1/responses"));
});

Deno.test("openai: prompt-cache capability records are UOS-only and keep providers separate", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const runtimeConfigKey = keyToString(["uos_ai", "runtime_config", "v2"]);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousRuntimeConfig = kvStore.get(runtimeConfigKey);
  const promptCache = {
    version: 1,
    providers: [
      {
        id: "codex_chatgpt",
        controls: {
          key: true,
          explicit_breakpoints: true,
          source: "catalog",
          verified_at_ms: 2_000,
        },
        scope: {
          probe_profile: "responses_explicit_input_text_keyed_30m",
          account_slots: "shared",
          token_refresh: "preserved",
          conversation_id: "independent",
          reproducible_cycles: 3,
          source: "live_probe",
          verified_at_ms: 2_001,
        },
      },
      {
        id: "yunwu",
        controls: {
          key: false,
          source: "inferred",
          verified_at_ms: 2_002,
        },
      },
    ],
  };
  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    client_version: "0.125.0",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      supported_reasoning_levels: ["none", "medium"],
      prompt_cache: promptCache,
    }],
  });

  try {
    const { capabilitiesResponse, modelsResponse } = await withFetchMock(
      () => {
        throw new Error("model metadata reads should not fetch upstream");
      },
      async () => ({
        capabilitiesResponse: await handleModelCapabilities(),
        modelsResponse: await handleModels(),
      }),
    );
    assert.equal(capabilitiesResponse.status, 200);
    const capabilities = await capabilitiesResponse.json() as {
      data?: Array<{ id?: string; prompt_cache?: unknown }>;
    };
    assert.deepEqual(
      capabilities.data?.find((model) => model.id === DEFAULT_TEST_MODEL)?.prompt_cache,
      promptCache,
    );

    const models = await modelsResponse.json() as { data?: Array<Record<string, unknown> & { id?: string }> };
    const model = models.data?.find((entry) => entry.id === DEFAULT_TEST_MODEL);
    assert.ok(model);
    assert.equal(Object.prototype.hasOwnProperty.call(model, "prompt_cache"), false);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousRuntimeConfig === undefined) kvStore.delete(runtimeConfigKey);
    else kvStore.set(runtimeConfigKey, previousRuntimeConfig);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: models returns an empty list when no snapshot is stored", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  kvStore.delete(snapshotKey);

  try {
    const response = await withFetchMock(
      () => {
        throw new Error("handleModels should not fetch upstream models");
      },
      () => handleModels(),
    );

    assert.equal(response.status, 200);
    const payload = await response.json() as { object?: string; data?: unknown[] };
    assert.equal(payload.object, "list");
    assert.deepEqual(payload.data, []);
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
  }
});

Deno.test("openai: unsupported snapshot model is rejected before upstream fetch", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("unsupported model requests should not fetch upstream");
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: "gpt-5-chat-latest",
            messages: [{ role: "user", content: "ping" }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 404);
  const payload = await response.json() as { error?: { message?: string; code?: string; param?: string | null } };
  assert.equal(payload.error?.code, "model_not_found");
  assert.equal(payload.error?.param, "model");
  assert.match(payload.error?.message ?? "", /Use \/v1\/models/);
});

Deno.test("openai: unlisted reasoning tiers pass through for upstream validation", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
            reasoning_effort: "minimal",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "minimal" });
});

Deno.test("openai: max reasoning is forwarded for models that support it", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
            reasoning_effort: "max",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
});

Deno.test("openai: catalog wire metadata maps Codex CLI ultra to upstream max", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  let recordedUserAgent: string | null = null;
  const response = await withFetchMock(
    (_url, bodyText, init) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      recordedUserAgent = new Headers(init?.headers).get("user-agent");
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "user", content: "ping" }],
            reasoning_effort: "ultra",
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
  assert.equal(recordedUserAgent, "codex_cli_rs/0.125.0 (ai.ubq.fi)");
});

Deno.test("openai: ultra still dispatches as max when a stored catalog has no wire map", async () => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const previousSnapshot = kvStore.get(snapshotKey);
  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      display_name: "No wire-map fixture",
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["none", "medium", "ultra"],
    }],
  });
  try {
    let recordedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              messages: [{ role: "user", content: "ping" }],
              reasoning_effort: "ultra",
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: responses applies catalog reasoning wire metadata", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            input: "ping",
            reasoning: { effort: "ultra" },
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).reasoning, { effort: "max" });
});

Deno.test("openai: Codex HTTP errors use OpenAI envelopes without changing routing", async (t) => {
  const cases = [
    {
      name: "chat completions parses a provider-root detail",
      route: "chat.completions",
      status: 400,
      statusText: "Codex Invalid Request",
      body: JSON.stringify({
        detail: "The requested model is not supported with a ChatGPT account.",
        opaque: { drop: true },
      }),
      retryAfter: "7",
      expectedError: {
        message: "The requested model is not supported with a ChatGPT account.",
        type: "invalid_request_error",
        code: "upstream_error",
      },
    },
    {
      name: "responses preserves an existing error envelope",
      route: "responses",
      status: 503,
      statusText: "Codex Unavailable",
      body: JSON.stringify({
        error: {
          message: "Codex is temporarily unavailable.",
          type: "server_error",
          code: "provider_unavailable",
          param: "model",
        },
        opaque: { drop: true },
      }),
      retryAfter: null,
      expectedError: {
        message: "Codex is temporarily unavailable.",
        type: "server_error",
        code: "provider_unavailable",
        param: "model",
      },
    },
    {
      name: "responses converts plain text",
      route: "responses",
      status: 422,
      statusText: "Codex Rejected",
      body: "Codex rejected the request body.",
      retryAfter: null,
      expectedError: {
        message: "Codex rejected the request body.",
        type: "invalid_request_error",
        code: "upstream_error",
      },
    },
  ] as const;

  for (const testCase of cases) {
    await t.step(testCase.name, async () => {
      let codexCalls = 0;
      const response = await withFetchMock(
        () => {
          codexCalls += 1;
          const headers = new Headers({
            "Content-Type": "application/problem+json",
            "X-Codex-Diagnostic": "drop-me",
          });
          if (testCase.retryAfter) headers.set("Retry-After", testCase.retryAfter);
          return new Response(testCase.body, {
            status: testCase.status,
            statusText: testCase.statusText,
            headers,
          });
        },
        () =>
          testCase.route === "chat.completions"
            ? handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  messages: [{ role: "user", content: "ping" }],
                }),
              }),
            )
            : handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
            ),
      );

      assert.equal(response.status, testCase.status);
      assert.equal(response.statusText, "");
      assert.equal(response.headers.get("Content-Type"), "application/json");
      assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
      assert.equal(response.headers.get("Retry-After"), testCase.retryAfter);
      assert.equal(response.headers.get("X-Codex-Diagnostic"), null);
      assert.deepEqual(await response.json(), { error: testCase.expectedError });
      assert.equal(codexCalls, 1);
    });
  }
});

Deno.test("openai: error normalization bounds oversized and stalled upstream bodies", async (t) => {
  const expectedError = {
    error: {
      message: "Upstream returned an oversized or incomplete error response.",
      type: "invalid_request_error",
      code: "upstream_error",
    },
  };

  await t.step("oversized bodies are cancelled without exposing partial content", async () => {
    let cancellations = 0;
    const oversized = new Uint8Array(65 * 1024);
    oversized.fill("x".charCodeAt(0));
    const startedAt = performance.now();
    const response = await withFetchMock(
      () =>
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(oversized);
            },
            cancel() {
              cancellations += 1;
            },
          }),
          {
            status: 422,
            headers: {
              "Content-Type": "application/problem+json",
              "X-Codex-Diagnostic": "drop-me",
            },
          },
        ),
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
          }),
        ),
    );

    assert.ok(performance.now() - startedAt < 500, "oversized body must be rejected before the reader deadline");
    assert.equal(response.status, 422);
    assert.equal(response.headers.get("Content-Type"), "application/json");
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.equal(response.headers.get("X-Codex-Diagnostic"), null);
    assert.deepEqual(await response.json(), expectedError);
    assert.equal(cancellations, 1);
  });

  setStreamFirstEventDeadlineMsForTest(100);
  try {
    for (const route of ["responses", "chat.completions"] as const) {
      await t.step(`${route} keeps the request deadline while reading an error body`, async () => {
        let cancellations = 0;
        const startedAt = performance.now();
        const response = await withFetchMock(
          () =>
            new Response(
              new ReadableStream<Uint8Array>({
                pull: () => new Promise<void>(() => {}),
                cancel() {
                  cancellations += 1;
                },
              }),
              { status: 400, headers: { "Content-Type": "application/problem+json" } },
            ),
          () =>
            route === "responses"
              ? handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
                }),
              )
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                    stream: true,
                  }),
                }),
              ),
        );

        assert.ok(performance.now() - startedAt < 500, route);
        assert.equal(response.status, 400);
        assert.deepEqual(await response.json(), expectedError);
        assert.equal(cancellations, 1);
      });
    }
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
  }
});

Deno.test("openai: a failed half-open 2xx stream releases its routing lease", async () => {
  let codexCalls = 0;
  await withFetchMock(
    () => {
      codexCalls += 1;
      if (codexCalls === 1) {
        return sseResponse([
          `data: ${
            JSON.stringify({ type: "response.created", response: { id: "resp_probe_failed", created_at: 0 } })
          }\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.failed",
              response: {
                id: "resp_probe_failed",
                status: "failed",
                model: DEFAULT_TEST_MODEL,
                output: [],
                usage: { input_tokens: 1, output_tokens: 0, total_tokens: 1 },
              },
            })
          }\n\n`,
        ]);
      }
      return sseResponse(baseSseChunks());
    },
    async () => {
      const pool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as {
        accounts: Array<{
          access_token: string;
          refresh_token: string;
          account_id: string;
        }>;
      };
      const account = pool.accounts[0]!;
      const credentialVersion = await sha256Hex(
        `${account.account_id}\u0000${account.access_token}\u0000${account.refresh_token}`,
      );
      kvStore.set(keyToString(["uos_ai", "codex_account_routing", "v2"]), {
        v: 2,
        updated_at_ms: Date.now(),
        slots: [{
          credential_version: credentialVersion,
          quota_blocked_until_ms: Date.now() - 1,
          quota_block_source: "header_retry_after",
          invalid_credential_version: null,
          primary_used_percent: null,
          secondary_used_percent: null,
          observed_reset_at_ms: Date.now() - 1,
          generation: 1,
          probe_lease: null,
        }],
      });

      const request = () =>
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "probe release" }),
        });
      const failed = await handleResponses(request());
      assert.equal(failed.status, 200);
      assert.equal((await failed.json() as { status?: string }).status, "failed");

      const second = await handleResponses(request());
      assert.equal(second.status, 200);
      assert.equal(second.headers.get("x-uos-upstream"), "chatgpt_codex");
      assert.equal(codexCalls, 2);
    },
  );
});

Deno.test("openai: gateway first-event deadlines return 504 on both streaming routes", async () => {
  setStreamFirstEventDeadlineMsForTest(10);
  try {
    for (const route of ["responses", "chat"] as const) {
      await withFetchMock(
        () =>
          new Response(
            new ReadableStream<Uint8Array>({
              start() {
                // Response headers arrive, but the upstream never emits an SSE event.
              },
            }),
            {
              status: 200,
              headers: { "Content-Type": "text/event-stream" },
            },
          ),
        async () => {
          const response = route === "responses"
            ? await handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
              }),
            )
            : await handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  messages: [{ role: "user", content: "ping" }],
                  stream: true,
                }),
              }),
            );
          const payload = await response.json() as { error?: { type?: unknown; code?: unknown } };
          assert.equal(response.status, 504, route);
          assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex", route);
          assert.equal(payload.error?.type, "server_error", route);
          assert.equal(payload.error?.code, "gateway_timeout", route);
          assert.equal(getResponseTelemetry(response)?.streamTerminalType, "deadline", route);
        },
      );
    }
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
  }
});

Deno.test("openai: Codex pre-header gateway deadlines use server_error on both streaming routes", async () => {
  setStreamFirstEventDeadlineMsForTest(10);
  try {
    for (const route of ["responses", "chat"] as const) {
      await withFetchMock(
        (_url, _bodyText, init) =>
          new Promise<Response>((_resolve, reject) => {
            const signal = init?.signal;
            if (!signal) {
              reject(new Error("Codex request did not receive a gateway deadline signal"));
              return;
            }
            const rejectWithAbortReason = () => reject(signal.reason);
            if (signal.aborted) rejectWithAbortReason();
            else signal.addEventListener("abort", rejectWithAbortReason, { once: true });
          }),
        async () => {
          const response = route === "responses"
            ? await handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
              }),
            )
            : await handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  messages: [{ role: "user", content: "ping" }],
                  stream: true,
                }),
              }),
            );
          const payload = await response.json() as { error?: { type?: unknown; code?: unknown } };
          assert.equal(response.status, 504, route);
          assert.equal(payload.error?.type, "server_error", route);
          assert.equal(payload.error?.code, "gateway_timeout", route);
        },
      );
    }
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
  }
});

Deno.test("openai: upstream fetch logs redact provider error payloads", async () => {
  const secret = "prompt-or-credential-must-not-reach-server-logs";
  const logs: unknown[][] = [];
  const originalConsoleError = console.error;
  console.error = (...args: unknown[]) => logs.push(args);

  try {
    for (const route of ["responses", "chat"] as const) {
      await withFetchMock(
        () => {
          const error = new TypeError(`provider echoed ${secret}`);
          (error as { cause?: unknown }).cause = { body: secret, message: secret };
          throw error;
        },
        async () => {
          const response = route === "responses"
            ? await handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
            )
            : await handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "ping" }] }),
              }),
            );
          assert.equal(response.status, 502, route);
          const payload = await response.json() as { error?: { code?: unknown } };
          assert.equal(payload.error?.code, "codex_upstream_unreachable", route);
        },
      );
    }
  } finally {
    console.error = originalConsoleError;
  }

  assert.equal(logs.length, 2);
  for (const args of logs) {
    assert.equal(args.length, 2);
    assert.equal(args[0], "[ai.ubq.fi] Upstream fetch failed:");
    assert.deepEqual(args[1], {
      error_class: "CodexError",
      status: 502,
      code: "codex_upstream_unreachable",
    });
    assert.equal(JSON.stringify(args).includes(secret), false);
  }
});

Deno.test("openai: YunWu paid fallback routing matrix", async (t) => {
  const originalApiKey = Deno.env.get("YUNWU_API_KEY");
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  try {
    await t.step("already-loaded disabled policy bypasses paid fallback reservation", async () => {
      const keyId = "fallback-policy-bypass";
      seedPaidFallbackKey(keyId, { enabled: true });
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId: "request-fallback-policy-bypass",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 429);
      assert.equal(calls, 2);
      const stored = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
        paid_fallback_reservation_request_id?: string | null;
      };
      assert.equal(stored.paid_fallback_reservation_request_id, null);
    });

    await t.step("disabled, unpriced, and exhausted keys retain the primary 429", async () => {
      const cases = [
        {
          id: "fallback-disabled",
          options: { enabled: false },
        },
        {
          id: "fallback-unpriced",
          options: { modelIds: ["some-other-model"] },
        },
        {
          id: "fallback-exhausted",
          options: { limitMicrocredits: 100, v3SettledMicrocredits: 100 },
        },
      ] as const;

      for (const testCase of cases) {
        seedPaidFallbackKey(testCase.id, testCase.options);
        let calls = 0;
        const response = await withFetchMock(
          () => {
            calls += 1;
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId: testCase.id,
                kernelRepo: null,
                kernelOrg: null,
                requestId: `request-${testCase.id}`,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 429, testCase.id);
        assert.equal(calls, 2);
        assert.deepEqual(await response.json(), {
          error: {
            message: "Primary limited",
            type: "rate_limit_error",
            code: "upstream_error",
          },
        });
      }
    });

    await t.step("fallback admission infrastructure failure retains the authoritative primary 429", async () => {
      const keyId = "fallback-admission-failure";
      seedPaidFallbackKey(keyId);
      atomicCommitFailure = (ops) =>
        ops.some((op) =>
            op.type === "set" &&
            op.key[0] === "uos_ai" &&
            op.key[1] === "paid_fallback" &&
            op.key[2] === "v3"
          )
          ? new Error("Enqueue operations are not supported in KV Connect")
          : null;
      let calls = 0;
      try {
        const response = await withFetchMock(
          () => {
            calls += 1;
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: {
                "Content-Type": "application/json",
                "Retry-After": "0",
              },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId: "request-fallback-admission-failure",
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 429);
        assert.equal(response.headers.get("Retry-After"), "0");
        assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
        assert.equal(calls, 2);
      } finally {
        atomicCommitFailure = null;
      }
    });

    await t.step("primary errors and network failures other than 429 never dispatch YunWu", async () => {
      for (const scenario of ["http_500", "network"] as const) {
        const keyId = `fallback-${scenario}`;
        seedPaidFallbackKey(keyId);
        let calls = 0;
        const response = await withFetchMock(
          () => {
            calls += 1;
            if (scenario === "network") throw new TypeError("primary network unavailable");
            return new Response(JSON.stringify({ error: { message: "Primary failed" } }), {
              status: 500,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId: `request-${keyId}`,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, scenario === "http_500" ? 500 : 502);
        assert.equal(calls, 1);
      }
    });

    await t.step("primary 403 selects YunWu once and emits only safe selection fields", async () => {
      const keyId = "fallback-primary-403";
      const requestId = "request-fallback-primary-403";
      seedPaidFallbackKey(keyId);
      const infoLogs: unknown[][] = [];
      const originalInfo = console.info;
      let codexCalls = 0;
      let yunwuCalls = 0;
      let selectionObservedBeforeYunwu = false;
      let primaryCancellationStarted = false;
      console.info = (...args: unknown[]) => infoLogs.push(args);
      const response = await (async () => {
        try {
          return await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                yunwuCalls += 1;
                selectionObservedBeforeYunwu = infoLogs.some((entry) => entry[0] === "[ai.ubq.fi] yunwu_selected");
                return sseResponse(baseSseChunks());
              }
              codexCalls += 1;
              return new Response(
                new ReadableStream<Uint8Array>({
                  start(controller) {
                    controller.enqueue(TEXT_ENCODER.encode('{"error":{"message":"do-not-log-primary-body"}}'));
                  },
                  cancel() {
                    primaryCancellationStarted = true;
                    return new Promise<void>(() => {});
                  },
                }),
                {
                  status: 403,
                  headers: { "Content-Type": "application/json" },
                },
              );
            },
            () =>
              handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "do-not-log-request-body" }),
                }),
                {
                  keyId,
                  kernelRepo: null,
                  kernelOrg: null,
                  requestId,
                  startedAtMs: Date.now(),
                },
              ),
          );
        } finally {
          console.info = originalInfo;
        }
      })();

      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
      assert.equal(getResponseTelemetry(response)?.fallbackReason, "primary_403");
      assert.equal(codexCalls, 1);
      assert.equal(yunwuCalls, 1);
      assert.equal(primaryCancellationStarted, true);
      assert.equal(selectionObservedBeforeYunwu, true);
      const selectionLogs = infoLogs.filter((entry) => entry[0] === "[ai.ubq.fi] yunwu_selected");
      assert.equal(selectionLogs.length, 1);
      assert.equal(typeof selectionLogs[0]?.[1], "string");
      const selectionPayload = JSON.parse(selectionLogs[0]?.[1] as string) as Record<string, unknown>;
      assert.deepEqual(selectionPayload, { request_id: requestId, reason: "primary_403" });
      assert.deepEqual(Object.keys(selectionPayload).sort(), ["reason", "request_id"]);
    });

    await t.step("cancellation before fallback admission creates no paid exposure", async () => {
      const keyId = "fallback-cancel-before-dispatch";
      const requestId = "request-fallback-cancel-before-dispatch";
      seedPaidFallbackKey(keyId);
      const controller = new AbortController();
      let codexCalls = 0;
      let yunwuCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://yunwu.ai/v1/responses") {
            yunwuCalls += 1;
            return sseResponse(baseSseChunks());
          }
          codexCalls += 1;
          return new Response(
            new ReadableStream<Uint8Array>({
              start(streamController) {
                streamController.enqueue(TEXT_ENCODER.encode('{"error":{"message":"Primary limited"}}'));
              },
              cancel() {
                controller.abort(new DOMException("client disconnected", "AbortError"));
              },
            }),
            {
              status: 429,
              headers: { "Content-Type": "application/json" },
            },
          );
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              signal: controller.signal,
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 502);
      assert.equal(codexCalls, 1);
      assert.equal(yunwuCalls, 0);
      assert.equal(getResponseTelemetry(response)?.provider, "chatgpt_codex");
      const stored = getStoredPaidFallbackRequest(keyId, requestId);
      assert.equal(stored, null);
      const keyRecord = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
        usage_reset_at_ms: number;
      };
      const window = kvStore.get(
        keyToString(["uos_ai", "paid_fallback", "v3", "window", keyId, keyRecord.usage_reset_at_ms]),
      ) as { reserved_microcredits?: number; pending_count?: number } | undefined;
      assert.equal(window, undefined);
    });

    await t.step("Responses sends the same canonical payload to YunWu exactly once", async () => {
      const keyId = "fallback-responses-success";
      seedPaidFallbackKey(keyId);
      const bodies: Record<string, unknown>[] = [];
      const urls: string[] = [];
      const response = await withFetchMock(
        (url, bodyText, init) => {
          urls.push(url);
          if (bodyText) bodies.push(JSON.parse(bodyText) as Record<string, unknown>);
          if (url === "https://yunwu.ai/v1/responses") {
            const stored = getStoredPaidFallbackRequest(
              keyId,
              "request-fallback-responses-success",
            );
            assert.equal(stored?.dispatch_state, "dispatched");
            assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer yunwu-test-key");
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "yunwu-responses-request",
              },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                input: "ping",
                reasoning: { effort: "ultra" },
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "request-fallback-responses-success",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
      assert.equal(getResponseTelemetry(response)?.quotaUsedPercent, 0);
      assert.equal(getResponseTelemetry(response)?.fallbackReason, "primary_429");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://chatgpt.com/backend-api/codex/responses",
        "https://yunwu.ai/v1/responses",
      ]);
      assert.equal(bodies.length, 3);
      assert.deepEqual(bodies[1], bodies[0]);
      assert.deepEqual(bodies[2], bodies[0]);
      assert.deepEqual(bodies[2].reasoning, { effort: "max" });
    });

    await t.step(
      "streaming Responses closes after YunWu's terminal event even when its socket stays open",
      async () => {
        const keyId = "fallback-responses-hanging-socket";
        seedPaidFallbackKey(keyId);
        let upstreamCancelled = false;
        const chunks = baseSseChunks();
        const terminalChunk = chunks.pop();
        assert.ok(terminalChunk);
        const crlfTerminalChunk = terminalChunk.replace(/\n/g, "\r\n");
        chunks.push(
          crlfTerminalChunk.slice(0, -1),
          `${crlfTerminalChunk.slice(-1)}: post-terminal bytes must not be forwarded\r\n\r\n`,
        );

        const responseText = await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              const body = new ReadableStream<Uint8Array>({
                start(controller) {
                  for (const chunk of chunks) controller.enqueue(TEXT_ENCODER.encode(chunk));
                },
                cancel() {
                  upstreamCancelled = true;
                },
              });
              return new Response(body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": "yunwu-hanging-socket-request",
                },
              });
            }
            if (url === "https://yunwu.ai/api/log/token") {
              return new Response(JSON.stringify({ success: true, data: [] }), {
                status: 200,
                headers: { "Content-Type": "application/json" },
              });
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          async () => {
            const response = await handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId: "request-fallback-responses-hanging-socket",
                startedAtMs: Date.now(),
              },
            );
            assert.equal(response.status, 200);
            assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
            return await response.text();
          },
        );

        assert.match(responseText, /"type":"response.completed"/);
        assert.doesNotMatch(responseText, /post-terminal/);
        assert.equal(upstreamCancelled, true);
      },
    );

    await t.step("Chat Completions also falls back through YunWu Responses once", async () => {
      const keyId = "fallback-chat-success";
      seedPaidFallbackKey(keyId);
      const urls: string[] = [];
      const response = await withFetchMock(
        (url) => {
          urls.push(url);
          if (url === "https://yunwu.ai/v1/responses") {
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "yunwu-chat-request",
              },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                messages: [{ role: "user", content: "ping" }],
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "request-fallback-chat-success",
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://chatgpt.com/backend-api/codex/responses",
        "https://yunwu.ai/v1/responses",
      ]);
    });

    await t.step("all recognized terminal events are recorded across routes and stream modes", async () => {
      const terminalCases = [
        { eventType: "response.completed", terminalState: "completed" },
        { eventType: "response.failed", terminalState: "failed" },
        { eventType: "response.incomplete", terminalState: "incomplete" },
        { eventType: "error", terminalState: "failed" },
      ] as const;
      const routeCases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;

      for (const routeCase of routeCases) {
        for (const terminalCase of terminalCases) {
          const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}-${
            terminalCase.eventType.replace(".", "-")
          }`;
          const keyId = `fallback-terminal-${suffix}`;
          const requestId = `request-${keyId}`;
          seedPaidFallbackKey(keyId);
          const terminalValue = terminalCase.eventType === "error"
            ? {
              type: "error",
              error: { type: "server_error", code: "provider_error", message: "provider failed" },
            }
            : {
              type: terminalCase.eventType,
              response: {
                id: `resp_${suffix}`,
                status: terminalCase.terminalState,
                model: DEFAULT_TEST_MODEL,
                output: [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            };

          await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                return new Response(
                  sseResponse([`data: ${JSON.stringify(terminalValue)}\n\n`]).body,
                  {
                    status: 200,
                    headers: {
                      "Content-Type": "text/event-stream",
                      "X-Api-Request-Id": `provider-${suffix}`,
                    },
                  },
                );
              }
              if (url.startsWith("https://yunwu.ai/api/log/token?")) {
                return new Response(JSON.stringify({ success: true, data: { items: [] } }), {
                  status: 200,
                  headers: { "Content-Type": "application/json" },
                });
              }
              return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
                status: 429,
                headers: { "Content-Type": "application/json" },
              });
            },
            async () => {
              const context = {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              };
              const response = routeCase.route === "responses"
                ? await handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      input: "ping",
                      stream: routeCase.stream,
                    }),
                  }),
                  context,
                )
                : await handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      stream: routeCase.stream,
                    }),
                  }),
                  context,
                );
              await response.text();
              const expectedStatus = routeCase.stream || terminalCase.eventType === "response.completed" ||
                  (routeCase.route === "responses" && terminalCase.eventType !== "error")
                ? 200
                : 502;
              assert.equal(response.status, expectedStatus, suffix);
              assert.equal(getResponseTelemetry(response)?.streamTerminalType, terminalCase.eventType, suffix);
              const stored = await waitForPaidFallbackTerminal(keyId, requestId, terminalCase.terminalState);
              assert.equal(stored.dispatch_state, "dispatched", suffix);
              assert.equal(stored.billing_state, "pending", suffix);
            },
          );
        }
      }
    });

    await t.step("YunWu network ambiguity returns an attributed 502 without retrying", async () => {
      const routeCases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      for (const routeCase of routeCases) {
        const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}`;
        const keyId = `fallback-network-error-${suffix}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        let yunwuAttempts = 0;
        await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              yunwuAttempts += 1;
              throw new TypeError("network connection reset before response headers");
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          async () => {
            const context = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            };
            const response = routeCase.route === "responses"
              ? await handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    input: "ping",
                    stream: routeCase.stream,
                  }),
                }),
                context,
              )
              : await handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                    stream: routeCase.stream,
                  }),
                }),
                context,
              );
            assert.equal(response.status, 502, suffix);
            assert.equal(response.headers.get("x-uos-upstream"), "yunwu", suffix);
            assert.equal(yunwuAttempts, 1, suffix);
            const payload = await response.json() as {
              error?: { type?: unknown; code?: unknown };
            };
            assert.equal(payload.error?.type, "server_error", suffix);
            assert.equal(payload.error?.code, "yunwu_upstream_unreachable", suffix);
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
            assert.equal(stored.dispatch_state, "dispatched", suffix);
            assert.equal(stored.provider_request_id, null, suffix);
            assert.equal(stored.billing_state, "pending", suffix);
          },
        );
      }
    });

    await t.step("YunWu pre-header deadlines return an attributed 504 without retrying", async () => {
      const routeCases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      for (const routeCase of routeCases) {
        const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}`;
        const keyId = `fallback-deadline-${suffix}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        const controller = new AbortController();
        let yunwuAttempts = 0;
        await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              yunwuAttempts += 1;
              controller.abort(new DOMException("gateway deadline exceeded", "TimeoutError"));
              throw controller.signal.reason;
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          async () => {
            const context = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            };
            const response = routeCase.route === "responses"
              ? await handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    input: "ping",
                    stream: routeCase.stream,
                  }),
                  signal: controller.signal,
                }),
                context,
              )
              : await handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                    stream: routeCase.stream,
                  }),
                  signal: controller.signal,
                }),
                context,
              );
            assert.equal(response.status, 504, suffix);
            assert.equal(response.headers.get("x-uos-upstream"), "yunwu", suffix);
            assert.equal(yunwuAttempts, 1, suffix);
            const payload = await response.json() as {
              error?: { type?: unknown; code?: unknown };
            };
            assert.equal(payload.error?.type, "server_error", suffix);
            assert.equal(payload.error?.code, "gateway_timeout", suffix);
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
            assert.equal(stored.dispatch_state, "dispatched", suffix);
            assert.equal(stored.provider_request_id, null, suffix);
            assert.equal(stored.billing_state, "pending", suffix);
          },
        );
      }
    });

    await t.step("missing YunWu bodies are recorded as ambiguous across routes and stream modes", async () => {
      const routeCases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      for (const routeCase of routeCases) {
        const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}`;
        const keyId = `fallback-missing-body-${suffix}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              return new Response(null, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Api-Request-Id": `provider-${suffix}`,
                },
              });
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          async () => {
            const context = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            };
            const response = routeCase.route === "responses"
              ? await handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    input: "ping",
                    stream: routeCase.stream,
                  }),
                }),
                context,
              )
              : await handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                    stream: routeCase.stream,
                  }),
                }),
                context,
              );
            assert.equal(response.status, 502, suffix);
            assert.equal(getResponseTelemetry(response)?.streamTerminalType, "error", suffix);
            await response.text();
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
            assert.equal(stored.billing_state, "pending", suffix);
          },
        );
      }
    });

    await t.step("premature EOF, malformed events, and reader errors remain billable and ambiguous", async () => {
      const routeCases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      const failureCases = [
        {
          name: "eof",
          terminalType: "eof",
          body: () => sseResponse(['data: {"type":"response.output_text.delta","delta":"partial"}\n\n']).body,
        },
        {
          name: "malformed",
          terminalType: "error",
          body: () => sseResponse(["data: not-json\n\n"]).body,
        },
        {
          name: "read-error",
          terminalType: "error",
          body: () =>
            new ReadableStream<Uint8Array>({
              pull(controller) {
                controller.error(new Error("provider socket reset"));
              },
            }),
        },
      ] as const;

      for (const routeCase of routeCases) {
        for (const failureCase of failureCases) {
          const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}-${failureCase.name}`;
          const keyId = `fallback-stream-failure-${suffix}`;
          const requestId = `request-${keyId}`;
          seedPaidFallbackKey(keyId);
          await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                return new Response(failureCase.body(), {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Api-Request-Id": `provider-${suffix}`,
                  },
                });
              }
              return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
                status: 429,
                headers: { "Content-Type": "application/json" },
              });
            },
            async () => {
              const context = {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              };
              const response = routeCase.route === "responses"
                ? await handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      input: "ping",
                      stream: routeCase.stream,
                    }),
                  }),
                  context,
                )
                : await handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      stream: routeCase.stream,
                    }),
                  }),
                  context,
                );
              const responseText = await response.text();
              const expectedStatus = routeCase.stream && failureCase.name === "eof" ? 200 : 502;
              assert.equal(response.status, expectedStatus, suffix);
              if (routeCase.stream) {
                assert.match(responseText, /upstream_stream_error/, suffix);
                if (routeCase.route === "chat" && failureCase.name === "eof") {
                  assert.match(responseText, /"error":\s*\{/, suffix);
                  assert.doesNotMatch(responseText, /\[DONE\]/, suffix);
                }
              }
              assert.equal(
                getResponseTelemetry(response)?.streamTerminalType,
                failureCase.terminalType,
                suffix,
              );
              const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
              assert.equal(stored.billing_state, "pending", suffix);
            },
          );
        }
      }
    });

    await t.step("downstream cancellation marks dispatched streaming requests cancelled", async () => {
      for (const route of ["responses", "chat"] as const) {
        const keyId = `fallback-downstream-cancel-${route}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        let upstreamCancelCount = 0;
        await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              const body = new ReadableStream<Uint8Array>({
                start(controller) {
                  controller.enqueue(
                    TEXT_ENCODER.encode(
                      'data: {"type":"response.created","response":{"id":"resp_cancel","created_at":1}}\n\n' +
                        'data: {"type":"response.output_text.delta","delta":"partial"}\n\n',
                    ),
                  );
                },
                cancel() {
                  upstreamCancelCount += 1;
                },
              });
              return new Response(body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Api-Request-Id": `provider-cancel-${route}`,
                },
              });
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          async () => {
            const context = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            };
            const response = route === "responses"
              ? await handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
                }),
                context,
              )
              : await handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                    stream: true,
                  }),
                }),
                context,
              );
            assert.equal(response.status, 200, route);
            assert.ok(response.body);
            const reader = response.body.getReader();
            const first = await reader.read();
            assert.equal(first.done, false, route);
            await reader.cancel("client disconnected");
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "cancelled");
            assert.equal(stored.dispatch_state, "dispatched", route);
            assert.equal(stored.billing_state, "pending", route);
            assert.equal(getResponseTelemetry(response)?.streamTerminalType, "cancelled", route);
          },
        );
        assert.equal(upstreamCancelCount, 1, route);
      }
    });

    await t.step("Chat streaming remains bounded until the downstream client pulls", async () => {
      const keyId = "fallback-chat-backpressure";
      const requestId = `request-${keyId}`;
      seedPaidFallbackKey(keyId);
      const providerChunks = Array.from(
        { length: 40 },
        (_, index) =>
          TEXT_ENCODER.encode(
            `data: ${JSON.stringify({ type: "response.output_text.delta", delta: String(index) })}\n\n`,
          ),
      );
      providerChunks.push(
        TEXT_ENCODER.encode(
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                status: "completed",
                model: DEFAULT_TEST_MODEL,
                output: [],
                usage: { input_tokens: 1, output_tokens: 40, total_tokens: 41 },
              },
            })
          }\n\n`,
        ),
      );
      let upstreamPullCount = 0;
      let upstreamCancelCount = 0;

      await withFetchMock(
        (url) => {
          if (url === "https://yunwu.ai/v1/responses") {
            const body = new ReadableStream<Uint8Array>({
              pull(controller) {
                const chunk = providerChunks[upstreamPullCount];
                upstreamPullCount += 1;
                if (chunk) controller.enqueue(chunk);
                else controller.close();
              },
              cancel() {
                upstreamCancelCount += 1;
              },
            });
            return new Response(body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Api-Request-Id": "provider-chat-backpressure",
              },
            });
          }
          return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        async () => {
          const response = await handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                messages: [{ role: "user", content: "ping" }],
                stream: true,
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            },
          );
          await Promise.resolve();
          await Promise.resolve();
          assert.ok(
            upstreamPullCount <= 3,
            `expected bounded upstream reads before downstream demand, received ${upstreamPullCount}`,
          );
          assert.ok(response.body);
          const reader = response.body.getReader();
          const first = await reader.read();
          assert.equal(first.done, false);
          await reader.cancel("stop after first translated chunk");
          await waitForPaidFallbackTerminal(keyId, requestId, "cancelled");
        },
      );
      assert.equal(upstreamCancelCount, 1);
    });

    await t.step("YunWu HTTP errors use OpenAI envelopes without changing routing", async (t) => {
      const cases = [
        {
          name: "responses preserves an existing error envelope and 429",
          route: "responses",
          status: 429,
          statusText: "YunWu Rate Limited",
          body: JSON.stringify({
            error: {
              message: "YunWu is rate limited.",
              type: "rate_limit_error",
              code: "provider_rate_limit",
              param: null,
            },
            opaque: { drop: true },
          }),
          retryAfter: "17",
          expectedError: {
            message: "YunWu is rate limited.",
            type: "rate_limit_error",
            code: "provider_rate_limit",
            param: null,
          },
        },
        {
          name: "chat completions parses a provider-root message and preserves 502",
          route: "chat.completions",
          status: 502,
          statusText: "YunWu Bad Gateway",
          body: JSON.stringify({
            message: "YunWu could not reach its model backend.",
            type: "server_error",
            code: "provider_unavailable",
            opaque: { drop: true },
          }),
          retryAfter: null,
          expectedError: {
            message: "YunWu could not reach its model backend.",
            type: "server_error",
            code: "provider_unavailable",
          },
        },
        {
          name: "chat completions converts plain text and preserves 401",
          route: "chat.completions",
          status: 401,
          statusText: "YunWu Unauthorized",
          body: "YunWu rejected the configured credential.",
          retryAfter: null,
          expectedError: {
            message: "YunWu rejected the configured credential.",
            type: "invalid_request_error",
            code: "upstream_error",
          },
        },
        {
          name: "responses classifies an untyped upstream 429 as rate limited",
          route: "responses",
          status: 429,
          statusText: "YunWu Rate Limited",
          body: JSON.stringify({ detail: "YunWu has no capacity." }),
          retryAfter: "3",
          expectedError: {
            message: "YunWu has no capacity.",
            type: "rate_limit_error",
            code: "upstream_error",
          },
        },
      ] as const;

      for (const [index, testCase] of cases.entries()) {
        await t.step(testCase.name, async () => {
          const keyId = `fallback-yunwu-normalized-${index}`;
          const requestId = `request-fallback-yunwu-normalized-${index}`;
          seedPaidFallbackKey(keyId);
          let codexCalls = 0;
          let yunwuCalls = 0;
          const response = await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                yunwuCalls += 1;
                const headers = new Headers({
                  "Content-Type": "application/problem+json",
                  "X-Yunwu-Diagnostic": "drop-me",
                });
                if (testCase.retryAfter) headers.set("Retry-After", testCase.retryAfter);
                return new Response(testCase.body, {
                  status: testCase.status,
                  statusText: testCase.statusText,
                  headers,
                });
              }
              codexCalls += 1;
              return new Response(JSON.stringify({ error: { message: "Primary forbidden" } }), {
                status: 403,
                headers: { "Content-Type": "application/json" },
              });
            },
            () =>
              testCase.route === "chat.completions"
                ? handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                    }),
                  }),
                  {
                    keyId,
                    kernelRepo: null,
                    kernelOrg: null,
                    requestId,
                    startedAtMs: Date.now(),
                  },
                )
                : handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
                  }),
                  {
                    keyId,
                    kernelRepo: null,
                    kernelOrg: null,
                    requestId,
                    startedAtMs: Date.now(),
                  },
                ),
          );

          assert.equal(response.status, testCase.status);
          assert.equal(response.statusText, "");
          assert.equal(response.headers.get("Content-Type"), "application/json");
          assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
          assert.equal(response.headers.get("Retry-After"), testCase.retryAfter);
          assert.equal(response.headers.get("X-Yunwu-Diagnostic"), null);
          assert.deepEqual(await response.json(), { error: testCase.expectedError });
          assert.equal(codexCalls, 1);
          assert.equal(yunwuCalls, 1);
          const failed = await waitForPaidFallbackTerminal(keyId, requestId, "failed");
          assert.equal(failed.terminal_state, "failed");
        });
      }
    });

    await t.step("a ledger write failure after YunWu accepts preserves the usable response", async () => {
      const keyId = "fallback-ledger-write-failure";
      const requestId = "request-fallback-ledger-write-failure";
      const providerRequestId = "yunwu-ledger-write-failure";
      seedPaidFallbackKey(keyId);
      atomicCommitFailure = (ops) =>
        ops.some((op) => {
            const value = op.value as { provider_request_id?: unknown } | undefined;
            return op.type === "set" && op.key[0] === "uos_ai" && op.key[1] === "paid_fallback" &&
              op.key[2] === "ledger" && value?.provider_request_id === providerRequestId;
          })
          ? new Error("injected paid fallback ledger failure")
          : null;
      try {
        const response = await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              return new Response(sseResponse(baseSseChunks()).body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 200);
        assert.match(await response.text(), /pong/);
        assert.equal(getResponseTelemetry(response)?.completed, true);
      } finally {
        atomicCommitFailure = null;
      }
    });

    await t.step("reconciliation failures preserve chat and Responses results across stream modes", async () => {
      const cases = [
        { route: "responses", stream: false },
        { route: "responses", stream: true },
        { route: "chat", stream: false },
        { route: "chat", stream: true },
      ] as const;
      exposePaidFallbackLedgerEntries = true;
      atomicCommitFailure = (ops) =>
        ops.some((op) => {
            const value = op.value as { billing_status?: unknown } | undefined;
            return op.type === "set" && op.key[0] === "uos_ai" && op.key[1] === "paid_fallback" &&
              op.key[2] === "ledger" && value?.billing_status === "reconciled";
          })
          ? new Error("injected paid fallback reconciliation failure")
          : null;
      try {
        for (const testCase of cases) {
          const suffix = `${testCase.route}-${testCase.stream ? "stream" : "nonstream"}`;
          const keyId = `fallback-reconcile-${suffix}`;
          const requestId = `request-fallback-reconcile-${suffix}`;
          const providerRequestId = `yunwu-reconcile-${suffix}`;
          seedPaidFallbackKey(keyId);
          const result = await withFetchMock(
            (url) => {
              if (url === "https://yunwu.ai/v1/responses") {
                return new Response(sseResponse(baseSseChunks()).body, {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Oneapi-Request-Id": providerRequestId,
                  },
                });
              }
              if (url === "https://yunwu.ai/api/log/token") {
                return new Response(
                  JSON.stringify({
                    success: true,
                    data: [{
                      request_id: providerRequestId,
                      quota: 100,
                      prompt_tokens: 1,
                      completion_tokens: 1,
                      model_name: DEFAULT_TEST_MODEL,
                      created_at: Math.floor(Date.now() / 1000),
                    }],
                  }),
                  { status: 200, headers: { "Content-Type": "application/json" } },
                );
              }
              return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
                status: 429,
                headers: { "Content-Type": "application/json" },
              });
            },
            async () => {
              const context = {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              };
              const response = await (testCase.route === "responses"
                ? handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      input: "ping",
                      stream: testCase.stream,
                    }),
                  }),
                  context,
                )
                : handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      stream: testCase.stream,
                    }),
                  }),
                  context,
                ));
              const completedBeforeConsumption = getResponseTelemetry(response)?.completed;
              const text = await response.text();
              return { response, text, completedBeforeConsumption };
            },
          );
          const { response, text, completedBeforeConsumption } = result;
          assert.equal(response.status, 200, suffix);
          if (testCase.stream) assert.equal(completedBeforeConsumption, false, suffix);
          assert.match(text, /pong/, suffix);
          assert.equal(getResponseTelemetry(response)?.completed, true, suffix);
        }
      } finally {
        atomicCommitFailure = null;
        exposePaidFallbackLedgerEntries = false;
      }
    });

    await t.step("reconciliation failure does not replace the original YunWu error", async () => {
      const keyId = "fallback-error-reconcile-failure";
      const requestId = "request-fallback-error-reconcile-failure";
      const providerRequestId = "yunwu-error-reconcile-failure";
      seedPaidFallbackKey(keyId);
      exposePaidFallbackLedgerEntries = true;
      atomicCommitFailure = (ops) =>
        ops.some((op) => (op.value as { billing_status?: unknown } | undefined)?.billing_status === "reconciled")
          ? new Error("injected upstream error reconciliation failure")
          : null;
      try {
        const response = await withFetchMock(
          (url) => {
            if (url === "https://yunwu.ai/v1/responses") {
              return new Response(JSON.stringify({ error: { message: "YunWu original error" } }), {
                status: 503,
                headers: {
                  "Content-Type": "application/json",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            if (url === "https://yunwu.ai/api/log/token") {
              return new Response(
                JSON.stringify({
                  success: true,
                  data: [{
                    request_id: providerRequestId,
                    quota: 100,
                    prompt_tokens: 1,
                    completion_tokens: 0,
                    model_name: DEFAULT_TEST_MODEL,
                    created_at: Math.floor(Date.now() / 1000),
                  }],
                }),
                { status: 200, headers: { "Content-Type": "application/json" } },
              );
            }
            return new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
              status: 429,
              headers: { "Content-Type": "application/json" },
            });
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
              }),
              {
                keyId,
                kernelRepo: null,
                kernelOrg: null,
                requestId,
                startedAtMs: Date.now(),
              },
            ),
        );
        assert.equal(response.status, 503);
        const payload = await response.json() as { error?: { message?: string } };
        assert.equal(payload.error?.message, "YunWu original error");
      } finally {
        atomicCommitFailure = null;
        exposePaidFallbackLedgerEntries = false;
      }
    });
  } finally {
    atomicCommitFailure = null;
    exposePaidFallbackLedgerEntries = false;
    if (originalApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalApiKey);
  }
});

Deno.test("http: CORS wrapper exposes a gateway request id", () => {
  const response = withCors(new Response("{}", { headers: { "Content-Type": "application/json" } }));
  assert.ok(response.headers.get("x-uos-request-id"));
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-request-id/);
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-upstream/);
});

Deno.test("http: CORS wrapper exposes baked source identity and deployment headers", () => {
  const originalGitRevision = Deno.env.get("GIT_REVISION");
  const originalGithubSha = Deno.env.get("GITHUB_SHA");
  const originalBuildId = Deno.env.get("DENO_DEPLOY_BUILD_ID");
  const originalDeploymentId = Deno.env.get("DENO_DEPLOYMENT_ID");
  try {
    Deno.env.set("GIT_REVISION", "git-test-revision");
    Deno.env.set("GITHUB_SHA", "github-test-sha");
    Deno.env.set("DENO_DEPLOY_BUILD_ID", "build-test-id");
    Deno.env.set("DENO_DEPLOYMENT_ID", "deployment-test-id");
    const response = withCors(new Response("{}", { headers: { "Content-Type": "application/json" } }));
    assert.equal(response.headers.get("x-uos-git-sha"), RELEASE_GIT_SHA);
    assert.equal(response.headers.get("x-uos-deployment-id"), "build-test-id");
    const exposed = response.headers.get("Access-Control-Expose-Headers") ?? "";
    assert.match(exposed, /x-uos-git-sha/);
    assert.match(exposed, /x-uos-deployment-id/);
  } finally {
    if (originalGitRevision === undefined) Deno.env.delete("GIT_REVISION");
    else Deno.env.set("GIT_REVISION", originalGitRevision);
    if (originalGithubSha === undefined) Deno.env.delete("GITHUB_SHA");
    else Deno.env.set("GITHUB_SHA", originalGithubSha);
    if (originalBuildId === undefined) Deno.env.delete("DENO_DEPLOY_BUILD_ID");
    else Deno.env.set("DENO_DEPLOY_BUILD_ID", originalBuildId);
    if (originalDeploymentId === undefined) Deno.env.delete("DENO_DEPLOYMENT_ID");
    else Deno.env.set("DENO_DEPLOYMENT_ID", originalDeploymentId);
  }
});

Deno.test("openai: normalize function-style tools for codex compatibility", async (t) => {
  await t.step("chat completions flattens tools and tool_choice", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              messages: [{ role: "user", content: "get weather" }],
              tools: [
                {
                  type: "function",
                  function: {
                    name: "fetch_weather",
                    description: "Fetch weather for a city.",
                    parameters: { type: "object", properties: { city: { type: "string" } } },
                  },
                },
                {
                  type: "function",
                  name: "legacy_tool",
                  description: "Already top-level tool name.",
                  parameters: { type: "object", properties: {} },
                  function: { strict: true },
                },
              ],
              tool_choice: {
                type: "function",
                name: "forced_choice",
                function: { name: "fetch_weather", strict: true },
              },
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    const recordedTools = recorded["tools"] as Array<Record<string, unknown>> | undefined;
    assert.ok(Array.isArray(recordedTools));
    assert.equal(recordedTools.length, 2);
    assert.equal(recordedTools[0]?.name, "fetch_weather");
    assert.equal(recordedTools[1]?.name, "legacy_tool");
    assert.equal(recordedTools[0]?.description, "Fetch weather for a city.");
    assert.deepEqual(recordedTools[0]?.parameters, {
      type: "object",
      properties: { city: { type: "string" } },
    });
    assert.equal(recordedTools[1]?.strict, true);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[0], "function"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[1], "function"), false);
    const recordedToolChoice = recorded["tool_choice"] as Record<string, unknown> | undefined;
    assert.ok(recordedToolChoice);
    assert.equal(recordedToolChoice.type, "function");
    assert.equal(recordedToolChoice["name"], "forced_choice");
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "strict"), false);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "function"), false);
  });

  await t.step("responses flattens tools", async () => {
    let recordedBody: Record<string, unknown> | null = null;

    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              input: "get weather",
              tools: [
                {
                  type: "function",
                  function: {
                    name: "fetch_weather",
                    description: "Fetch weather for a city.",
                    parameters: { type: "object", properties: { city: { type: "string" } } },
                  },
                },
              ],
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    const recordedTools = recorded["tools"] as Array<Record<string, unknown>> | undefined;
    assert.ok(Array.isArray(recordedTools));
    assert.equal(recordedTools.length, 1);
    assert.equal(recordedTools[0]?.name, "fetch_weather");
    assert.equal(recordedTools[0]?.description, "Fetch weather for a city.");
    assert.deepEqual(recordedTools[0]?.parameters, { type: "object", properties: { city: { type: "string" } } });
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[0], "function"), false);
  });
});

Deno.test("openai: chat completions accept system-only messages", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            messages: [{ role: "system", content: "Only system." }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal(recorded["instructions"], "Only system.");
  const input = recorded["input"];
  assert.ok(Array.isArray(input));
  assert.ok(input.length > 0);
  const first = input[0] as Record<string, unknown>;
  assert.equal(first["type"], "message");
  assert.equal(first["role"], "user");
  const content = first["content"];
  assert.ok(Array.isArray(content));
  const firstContent = (content as Record<string, unknown>[])[0] ?? null;
  assert.equal(firstContent?.["type"], "input_text");
});

Deno.test("openai: responses accept non-message input items", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            input: [
              {
                type: "message",
                role: "user",
                content: [{ type: "input_text", text: "ping" }],
              },
              {
                type: "message",
                role: "assistant",
                content: [{ type: "output_text", text: "Prior answer", annotations: [] }],
              },
              { type: "reasoning", summary: "thinking..." },
              { type: "function_call", name: "test", call_id: "call_1", arguments: "{}" },
              { type: "function_call_output", call_id: "call_1", output: "ok" },
            ],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);

  const input = recordedBody["input"];
  assert.ok(Array.isArray(input));

  const types = (input as Array<Record<string, unknown>>)
    .map((item) => (item && typeof item === "object") ? item["type"] : null)
    .filter((value): value is string => typeof value === "string");

  assert.ok(types.includes("reasoning"));
  assert.ok(types.includes("function_call"));
  assert.ok(types.includes("function_call_output"));
  const assistant = (input as Array<Record<string, unknown>>).find((item) => item.role === "assistant");
  assert.deepEqual(assistant?.content, [{ type: "output_text", text: "Prior answer" }]);
});

Deno.test("openai: buffered responses preserve function calls emitted as output items", async () => {
  const functionCall = {
    id: "fc_test",
    type: "function_call",
    status: "completed",
    name: "assistant_exports_download",
    call_id: "call_export",
    arguments: '{"format":"csv"}',
  };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.output_item.done",
            output_index: 0,
            item: functionCall,
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              model: DEFAULT_TEST_MODEL,
              output: [],
              usage: { input_tokens: 10, output_tokens: 20, total_tokens: 30 },
            },
          })
        }\n\n`,
      ]),
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            input: "export csv",
            tools: [{
              type: "function",
              name: "assistant_exports_download",
              description: "Download the selected records.",
              parameters: {
                type: "object",
                properties: { format: { type: "string", enum: ["csv", "json"] } },
                required: ["format"],
                additionalProperties: false,
              },
              strict: true,
            }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const payload = await response.json() as { output?: unknown[] };
  assert.deepEqual(payload.output, [functionCall]);
});

Deno.test("openai: responses preserve image detail on normalized input images", async () => {
  let recordedBody: Record<string, unknown> | null = null;

  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            reasoning: { effort: "low" },
            input: [
              {
                role: "user",
                content: [
                  { type: "input_text", text: "Read this." },
                  {
                    type: "input_image",
                    image_url: "data:image/jpeg;base64,/9j/4AAQ",
                    detail: "high",
                  },
                ],
              },
            ],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const input = recordedBody["input"];
  assert.ok(Array.isArray(input));
  const message = (input as Record<string, unknown>[])[0];
  const content = message?.content;
  assert.ok(Array.isArray(content));
  const image = (content as Record<string, unknown>[]).find((part) => part.type === "input_image");
  assert.equal(image?.image_url, "data:image/jpeg;base64,/9j/4AAQ");
  assert.equal(image?.detail, "high");
});

Deno.test("openai: Chat tool conversations retain tool-call order and opaque arguments", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  let upstreamCalls = 0;
  const response = await withFetchMock(
    (_url, bodyText) => {
      upstreamCalls += 1;
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [
              { role: "user", content: "Schedule it." },
              {
                role: "assistant",
                content: "I need two details.",
                tool_calls: [
                  { id: "call_calendar", type: "function", function: { name: "calendar", arguments: " { bad json" } },
                  { id: "call_weather", type: "function", function: { name: "weather", arguments: "{}" } },
                ],
              },
              { role: "tool", tool_call_id: "call_calendar", content: "Calendar is free." },
              {
                role: "tool",
                tool_call_id: "call_weather",
                content: [{ type: "text", text: "Sunny" }, { type: "text", text: " and warm" }],
              },
            ],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.equal(upstreamCalls, 1);
  assert.ok(recordedBody);
  const input = recordedBody["input"] as Array<Record<string, unknown>>;
  assert.deepEqual(input.map((item) => item.type), [
    "message",
    "message",
    "function_call",
    "function_call",
    "function_call_output",
    "function_call_output",
  ]);
  assert.deepEqual(input[2], {
    type: "function_call",
    call_id: "call_calendar",
    name: "calendar",
    arguments: " { bad json",
  });
  assert.deepEqual(input[5], {
    type: "function_call_output",
    call_id: "call_weather",
    output: [{ type: "input_text", text: "Sunny" }, { type: "input_text", text: " and warm" }],
  });
});

Deno.test("openai: Chat assistant refusal content replays as output text", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{ role: "assistant", content: [{ type: "refusal", refusal: "Cannot help." }] }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const requestBody = recordedBody as Record<string, unknown>;
  assert.deepEqual(requestBody.input, [{
    type: "message",
    role: "assistant",
    content: [{ type: "output_text", text: "Cannot help." }],
  }]);
});

Deno.test("openai: malformed Chat tool calls are rejected before provider dispatch", async () => {
  let upstreamCalls = 0;
  const response = await withFetchMock(
    () => {
      upstreamCalls += 1;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{
              role: "assistant",
              content: null,
              tool_calls: [{ id: "call_bad", type: "function", function: { name: "bad", arguments: {} } }],
            }],
          }),
        }),
      ),
  );
  assert.equal(response.status, 400);
  assert.equal(upstreamCalls, 0);
  const payload = await response.json() as { error?: { param?: string } };
  assert.equal(payload.error?.param, "messages[0].tool_calls[0].function.arguments");
});

Deno.test("openai: Chat accepts a tool-call-only assistant message with omitted content", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{
              role: "assistant",
              tool_calls: [{ id: "call_omitted", type: "function", function: { name: "lookup", arguments: "{}" } }],
            }],
          }),
        }),
      ),
  );
  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.deepEqual((recordedBody as Record<string, unknown>).input, [{
    type: "function_call",
    call_id: "call_omitted",
    name: "lookup",
    arguments: "{}",
  }]);
});

Deno.test("openai: Chat function calls translate consistently in buffered and streamed output", async (t) => {
  const callOne = {
    id: "fc_1",
    type: "function_call",
    call_id: "call_one",
    name: "first",
    arguments: '{"a":1}',
  };
  const callTwo = {
    id: "fc_2",
    type: "function_call",
    call_id: "call_two",
    name: "second",
    arguments: '{"b":2}',
  };
  const chunks = [
    `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_calls", created_at: 1 } })}\n\n`,
    `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "Before tools. " })}\n\n`,
    `data: ${JSON.stringify({ type: "response.output_item.added", output_index: 9, item: callOne })}\n\n`,
    `data: ${JSON.stringify({ type: "response.output_item.added", output_index: 9, item: callOne })}\n\n`,
    `data: ${
      JSON.stringify({ type: "response.output_item.added", output_index: 4, item: { ...callTwo, arguments: "" } })
    }\n\n`,
    `data: ${JSON.stringify({ type: "response.function_call_arguments.delta", item_id: "fc_2", delta: '{"b":' })}\n\n`,
    `data: ${
      JSON.stringify({ type: "response.function_call_arguments.done", item_id: "fc_2", arguments: '{"b":2}' })
    }\n\n`,
    `data: ${JSON.stringify({ type: "response.output_item.done", output_index: 9, item: callOne })}\n\n`,
    `data: ${JSON.stringify({ type: "response.output_item.done", output_index: 4, item: callTwo })}\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.completed",
        response: {
          output: [callOne, callTwo],
          usage: { input_tokens: 1, output_tokens: 2, total_tokens: 3 },
        },
      })
    }\n\n`,
  ];

  await t.step("buffered", async () => {
    const response = await withFetchMock(
      () => sseResponse(chunks),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "tools" }] }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    const payload = await response.json() as {
      choices: Array<{ message: Record<string, unknown>; finish_reason: string }>;
    };
    assert.equal(payload.choices[0]?.finish_reason, "tool_calls");
    assert.equal(payload.choices[0]?.message.content, "Before tools. ");
    assert.deepEqual(payload.choices[0]?.message.tool_calls, [
      { id: "call_one", type: "function", function: { name: "first", arguments: '{"a":1}' } },
      { id: "call_two", type: "function", function: { name: "second", arguments: '{"b":2}' } },
    ]);
  });

  await t.step("streamed", async () => {
    const response = await withFetchMock(
      () => sseResponse(chunks),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              stream: true,
              messages: [{ role: "user", content: "tools" }],
            }),
          }),
        ),
    );
    const text = await response.text();
    assert.match(text, /"tool_calls"/);
    assert.match(text, /"finish_reason":"tool_calls"/);
    assert.match(text, /data: \[DONE\]/);
    const toolArgumentDeltas = text
      .split("\n\n")
      .filter((frame) => frame.startsWith("data: {") && frame.includes("tool_calls"))
      .flatMap((frame) => {
        const payload = JSON.parse(frame.slice("data: ".length)) as {
          choices?: Array<{ delta?: { tool_calls?: Array<{ function?: { arguments?: string } }> } }>;
        };
        return payload.choices?.flatMap((choice) => choice.delta?.tool_calls ?? []) ?? [];
      })
      .map((call) => call.function?.arguments);
    // A duplicate added event does not replay its complete argument string.
    assert.deepEqual(toolArgumentDeltas, ['{"a":1}', "", '{"b":', "2}"]);
  });
});

Deno.test("openai: inconsistent function-call stream arguments never emit Chat [DONE]", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.output_item.added",
            output_index: 0,
            item: { id: "fc_bad", type: "function_call", call_id: "call_bad", name: "bad", arguments: "" },
          })
        }\n\n`,
        `data: ${
          JSON.stringify({ type: "response.function_call_arguments.delta", item_id: "fc_bad", delta: "{" })
        }\n\n`,
        `data: ${
          JSON.stringify({ type: "response.function_call_arguments.done", item_id: "fc_bad", arguments: "[]" })
        }\n\n`,
      ]),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            stream: true,
            messages: [{ role: "user", content: "tools" }],
          }),
        }),
      ),
  );
  const text = await response.text();
  assert.match(text, /upstream_stream_error/);
  assert.doesNotMatch(text, /data: \[DONE\]/);
});

Deno.test("openai: terminal function calls without arguments fail for buffered and streamed Chat", async (t) => {
  const malformedEvents = [
    `data: ${
      JSON.stringify({
        type: "response.output_item.added",
        output_index: 0,
        item: { id: "fc_missing_args", type: "function_call", call_id: "call_missing_args", name: "bad" },
      })
    }\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.output_item.done",
        output_index: 0,
        item: { id: "fc_missing_args", type: "function_call", call_id: "call_missing_args", name: "bad" },
      })
    }\n\n`,
  ];
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(malformedEvents),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "tools" }],
              }),
            }),
          ),
      );
      if (stream) {
        const text = await response.text();
        assert.match(text, /upstream_stream_error/);
        assert.doesNotMatch(text, /data: \[DONE\]/);
      } else {
        assert.equal(response.status, 502);
      }
    });
  }
});

Deno.test("openai: late function-call argument deltas never produce a successful Chat terminal", async (t) => {
  const malformedEvents = [
    `data: ${
      JSON.stringify({
        type: "response.output_item.added",
        output_index: 0,
        item: {
          id: "fc_late_delta",
          type: "function_call",
          call_id: "call_late_delta",
          name: "bad",
          arguments: "",
        },
      })
    }\n\n`,
    `data: ${
      JSON.stringify({ type: "response.function_call_arguments.done", item_id: "fc_late_delta", arguments: "{}" })
    }\n\n`,
    `data: ${
      JSON.stringify({ type: "response.function_call_arguments.delta", item_id: "fc_late_delta", delta: "x" })
    }\n\n`,
    `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
  ];
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(malformedEvents),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "tools" }],
              }),
            }),
          ),
      );
      if (stream) {
        const text = await response.text();
        assert.match(text, /upstream_stream_error/);
        assert.doesNotMatch(text, /data: \[DONE\]/);
      } else {
        assert.equal(response.status, 502);
      }
    });
  }
});

Deno.test("openai: unfinished function calls never produce a successful Chat terminal", async (t) => {
  const malformedEvents = [
    `data: ${
      JSON.stringify({
        type: "response.output_item.added",
        output_index: 0,
        item: { id: "fc_unfinished", type: "function_call", call_id: "call_unfinished", name: "bad" },
      })
    }\n\n`,
    `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
  ];
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(malformedEvents),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "tools" }],
              }),
            }),
          ),
      );
      if (stream) {
        const text = await response.text();
        assert.match(text, /upstream_stream_error/);
        assert.doesNotMatch(text, /data: \[DONE\]/);
      } else {
        assert.equal(response.status, 502);
      }
    });
  }
});

Deno.test("openai: malformed output-text deltas never produce a successful Chat terminal", async (t) => {
  const malformedEvents = [
    `data: ${JSON.stringify({ type: "response.output_text.delta", delta: null })}\n\n`,
    `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
  ];
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(malformedEvents),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "text" }],
              }),
            }),
          ),
      );
      if (stream) {
        const text = await response.text();
        assert.match(text, /upstream_stream_error/);
        assert.doesNotMatch(text, /data: \[DONE\]/);
      } else {
        assert.equal(response.status, 502);
      }
    });
  }
});

Deno.test("openai: tool-call-only buffered Chat output uses null content", async () => {
  const call = { id: "fc_only", type: "function_call", call_id: "call_only", name: "only", arguments: "{}" };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${JSON.stringify({ type: "response.output_item.done", output_index: 0, item: call })}\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [call] } })}\n\n`,
      ]),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "tools" }] }),
        }),
      ),
  );
  const payload = await response.json() as { choices: Array<{ message: { content: unknown }; finish_reason: string }> };
  assert.equal(payload.choices[0]?.message.content, null);
  assert.equal(payload.choices[0]?.finish_reason, "tool_calls");
});

Deno.test("openai: buffered Chat preserves final-only text alongside function calls", async () => {
  const call = {
    id: "fc_final_text",
    type: "function_call",
    call_id: "call_final_text",
    name: "lookup",
    arguments: "{}",
  };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              output: [
                {
                  id: "msg_final_text",
                  type: "message",
                  role: "assistant",
                  content: [{ type: "output_text", text: "I will look that up." }],
                },
                call,
              ],
            },
          })
        }\n\n`,
      ]),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "tools" }] }),
        }),
      ),
  );
  assert.equal(response.status, 200);
  const payload = await response.json() as {
    choices?: Array<{ message?: { content?: unknown; tool_calls?: unknown }; finish_reason?: string }>;
  };
  assert.equal(payload.choices?.[0]?.finish_reason, "tool_calls");
  assert.equal(payload.choices?.[0]?.message?.content, "I will look that up.");
  assert.deepEqual(payload.choices?.[0]?.message?.tool_calls, [
    { id: "call_final_text", type: "function", function: { name: "lookup", arguments: "{}" } },
  ]);
});

Deno.test("openai: buffered Chat preserves final text from response.output", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.output",
            output: [{
              id: "msg_output_event",
              type: "message",
              role: "assistant",
              content: [{ type: "output_text", text: "Text supplied by response.output." }],
            }],
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ]),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "text" }] }),
        }),
      ),
  );
  assert.equal(response.status, 200);
  const payload = await response.json() as { choices?: Array<{ message?: { content?: unknown } }> };
  assert.equal(payload.choices?.[0]?.message?.content, "Text supplied by response.output.");
});

Deno.test("openai: streamed Chat preserves final-only text alongside function calls", async () => {
  const call = {
    id: "fc_stream_final_text",
    type: "function_call",
    call_id: "call_stream_final_text",
    name: "lookup",
    arguments: "{}",
  };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              output: [
                {
                  id: "msg_stream_final_text",
                  type: "message",
                  role: "assistant",
                  content: [{ type: "output_text", text: "I will look that up." }],
                },
                call,
              ],
            },
          })
        }\n\n`,
      ]),
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            stream: true,
            messages: [{ role: "user", content: "tools" }],
          }),
        }),
      ),
  );
  const text = await response.text();
  assert.match(text, /I will look that up\./);
  assert.match(text, /"tool_calls"/);
  assert.match(text, /"finish_reason":"tool_calls"/);
  assert.match(text, /data: \[DONE\]/);
});

Deno.test("openai: native Responses preserve files, explicit nulls, and prompt-cache fields", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            instructions: null,
            prompt_cache_key: "cache-key",
            prompt_cache_options: { mode: "implicit" },
            prompt_cache_retention: "24h",
            input: [{
              type: "message",
              role: "user",
              content: [
                { type: "input_image", file_id: "file_image", detail: null },
                {
                  type: "input_file",
                  file_id: "file_id",
                  file_data: "data",
                  file_url: "https://example.test/file",
                  filename: null,
                },
              ],
            }],
          }),
        }),
      ),
  );
  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal("instructions" in recorded, false);
  assert.deepEqual(recorded.prompt_cache_options, { mode: "implicit" });
  assert.equal(recorded.prompt_cache_retention, "24h");
  const content = ((recorded.input as Array<Record<string, unknown>>)[0]?.content ?? []) as Array<
    Record<string, unknown>
  >;
  assert.deepEqual(content[0], { type: "input_image", file_id: "file_image", detail: null });
  assert.deepEqual(content[1], {
    type: "input_file",
    file_id: "file_id",
    file_data: "data",
    file_url: "https://example.test/file",
    filename: null,
  });
});

Deno.test("openai: cache usage parser retains observed values and marks malformed fields invalid", () => {
  assert.deepEqual(
    extractUsageTokens({
      input_tokens: 10,
      input_tokens_details: { cached_tokens: 0 },
      output_tokens: 2,
      total_tokens: 12,
    }),
    {
      inputTokens: 10,
      cachedInputTokens: 0,
      cacheWriteInputTokens: null,
      outputTokens: 2,
      totalTokens: 12,
      status: "reported",
    },
  );

  for (const nestedCacheValue of [-1, Number.POSITIVE_INFINITY]) {
    assert.deepEqual(
      extractUsageTokens({
        input_tokens: 10,
        input_tokens_details: { cached_tokens: nestedCacheValue, cache_write_tokens: 0 },
        output_tokens: 2,
        total_tokens: 12,
      }),
      {
        inputTokens: 10,
        cachedInputTokens: null,
        cacheWriteInputTokens: 0,
        outputTokens: 2,
        totalTokens: 12,
        status: "invalid",
      },
    );
  }

  assert.deepEqual(
    extractUsageTokens({
      input_tokens: 10,
      input_tokens_details: { cached_tokens: 11, cache_write_tokens: 0 },
      output_tokens: 2,
      total_tokens: 12,
    }),
    {
      inputTokens: 10,
      cachedInputTokens: 11,
      cacheWriteInputTokens: 0,
      outputTokens: 2,
      totalTokens: 12,
      status: "invalid",
    },
  );

  assert.deepEqual(
    extractUsageTokens({
      input_tokens: 10,
      input_tokens_details: { cached_tokens: 8, cache_write_tokens: 3 },
      output_tokens: 2,
      total_tokens: Number.NaN,
    }),
    {
      inputTokens: 10,
      cachedInputTokens: 8,
      cacheWriteInputTokens: 3,
      outputTokens: 2,
      totalTokens: null,
      status: "invalid",
    },
  );
});

Deno.test("openai: cache token usage reaches Chat clients and internal telemetry", async (t) => {
  const usage = {
    input_tokens: 2006,
    input_tokens_details: { cached_tokens: 1920, cache_write_tokens: 0 },
    output_tokens: 300,
    total_tokens: 2306,
  };
  const completed = () =>
    sseResponse([
      `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_cache", created_at: 1 } })}\n\n`,
      `data: ${
        JSON.stringify({ type: "response.completed", response: { model: DEFAULT_TEST_MODEL, output: [], usage } })
      }\n\n`,
    ]);

  await t.step("ttl-only cache options report the documented implicit mode", async () => {
    const response = await withFetchMock(
      () => completed(),
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              prompt_cache_options: { ttl: "30m" },
              input: "ttl-only cache policy",
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.equal(getResponseTelemetry(response)?.promptCacheMode, "implicit");
  });

  await t.step("buffered Chat maps standard usage details", async () => {
    const response = await withFetchMock(
      () => completed(),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "ping" }] }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    const body = await response.json() as { usage?: Record<string, unknown> };
    assert.deepEqual(body.usage, {
      prompt_tokens: 2006,
      completion_tokens: 300,
      total_tokens: 2306,
      prompt_tokens_details: { cached_tokens: 1920, cache_write_tokens: 0 },
    });
    assert.deepEqual(getResponseTelemetry(response), {
      provider: "chatgpt_codex",
      fallbackReason: null,
      model: DEFAULT_TEST_MODEL,
      reasoning: "low",
      inputTokens: 2006,
      cachedInputTokens: 1920,
      cacheWriteInputTokens: 0,
      outputTokens: 300,
      totalTokens: 2306,
      usageObserved: true,
      usageTelemetryStatus: "reported",
      promptCacheKeyPresent: false,
      promptCacheMode: "unspecified",
      explicitBreakpointCount: 0,
      accountSlot: 1,
      affinityOutcome: "none",
      quotaUsedPercent: undefined,
      completed: true,
      streamTerminalType: "response.completed",
      stream: false,
      firstCodexDispatchMs: null,
      firstCodexHeadersMs: null,
      firstSseEventMs: null,
      streamTerminalMs: null,
    });
  });

  await t.step("streamed Chat emits the standard final usage chunk only when requested", async () => {
    const response = await withFetchMock(
      () => completed(),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              stream: true,
              stream_options: { include_usage: true },
              messages: [{ role: "user", content: "ping" }],
            }),
          }),
        ),
    );
    const text = await response.text();
    const usageChunk = text.split("\n\n").find((chunk) => chunk.includes('"choices":[]'));
    assert.ok(usageChunk);
    assert.match(usageChunk, /"cached_tokens":1920/);
    assert.match(usageChunk, /"cache_write_tokens":0/);
    assert.ok(text.indexOf(usageChunk) < text.indexOf("data: [DONE]"));
  });

  await t.step("streamed Chat records cache telemetry without a requested public usage chunk", async () => {
    const response = await withFetchMock(
      () => completed(),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              stream: true,
              messages: [{ role: "user", content: "ping" }],
            }),
          }),
        ),
    );
    const text = await response.text();
    assert.doesNotMatch(text, /"choices":\[\]/);
    assert.equal(getResponseTelemetry(response)?.cachedInputTokens, 1920);
    assert.equal(getResponseTelemetry(response)?.cacheWriteInputTokens, 0);
    assert.equal(getResponseTelemetry(response)?.usageTelemetryStatus, "reported");
  });

  await t.step("streamed Responses preserves cache usage bytes while recording the same telemetry", async () => {
    const response = await withFetchMock(
      () => completed(),
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, stream: true, input: "ping" }),
          }),
        ),
    );
    const text = await response.text();
    assert.match(text, /"cached_tokens":1920/);
    assert.match(text, /"cache_write_tokens":0/);
    assert.equal(getResponseTelemetry(response)?.cachedInputTokens, 1920);
    assert.equal(getResponseTelemetry(response)?.cacheWriteInputTokens, 0);
    assert.equal(getResponseTelemetry(response)?.usageTelemetryStatus, "reported");
  });

  await t.step(
    "failed, partial, and invalid provider usage remains observable without fabricating public values",
    async () => {
      const failed = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.failed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  status: "failed",
                  usage: {
                    input_tokens: 100,
                    input_tokens_details: { cached_tokens: 80, cache_write_tokens: 0 },
                    output_tokens: 0,
                    total_tokens: 100,
                  },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "fail" }),
            }),
          ),
      );
      assert.equal(failed.status, 200);
      assert.deepEqual(getResponseTelemetry(failed), {
        ...getResponseTelemetry(failed),
        completed: false,
        inputTokens: 100,
        cachedInputTokens: 80,
        cacheWriteInputTokens: 0,
        outputTokens: 0,
        totalTokens: 100,
        usageObserved: true,
        usageTelemetryStatus: "reported",
        streamTerminalType: "response.failed",
      });

      const partial = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: { input_tokens: 11, input_tokens_details: { cached_tokens: 10 } },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, messages: [{ role: "user", content: "partial" }] }),
            }),
          ),
      );
      assert.equal((await partial.json() as { usage?: unknown }).usage, undefined);
      assert.equal(getResponseTelemetry(partial)?.cachedInputTokens, 10);
      assert.equal(getResponseTelemetry(partial)?.usageTelemetryStatus, "partial");

      const absentCachedTokens = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: { input_tokens: 11, output_tokens: 0, total_tokens: 11 },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "missing cache detail" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(absentCachedTokens)?.usageObserved, true);
      assert.equal(getResponseTelemetry(absentCachedTokens)?.cachedInputTokens, null);
      assert.equal(getResponseTelemetry(absentCachedTokens)?.usageTelemetryStatus, "partial");

      const absentUsage = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: { model: DEFAULT_TEST_MODEL, output: [] },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "absent usage" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(absentUsage)?.usageObserved, false);
      assert.equal(getResponseTelemetry(absentUsage)?.usageTelemetryStatus, "missing");
      assert.equal("usage" in (await absentUsage.json() as Record<string, unknown>), false);

      const cacheReadAboveInput = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: {
                    input_tokens: 10,
                    input_tokens_details: { cached_tokens: 11 },
                    output_tokens: 0,
                    total_tokens: 10,
                  },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "cache read above input" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(cacheReadAboveInput)?.cachedInputTokens, 11);
      assert.equal(getResponseTelemetry(cacheReadAboveInput)?.usageTelemetryStatus, "invalid");

      const overlappingCacheAccounting = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: {
                    input_tokens: 100,
                    input_tokens_details: { cached_tokens: 80, cache_write_tokens: 80 },
                    output_tokens: 0,
                    total_tokens: 100,
                  },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "overlapping cache accounting" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(overlappingCacheAccounting)?.cachedInputTokens, 80);
      assert.equal(getResponseTelemetry(overlappingCacheAccounting)?.cacheWriteInputTokens, 80);
      assert.equal(getResponseTelemetry(overlappingCacheAccounting)?.usageTelemetryStatus, "reported");

      const inconsistentUsage = {
        input_tokens: 10,
        input_tokens_details: { cached_tokens: 0, cache_write_tokens: 0 },
        output_tokens: 1,
        total_tokens: 12,
      };
      const inconsistentTotals = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: { model: DEFAULT_TEST_MODEL, output: [], usage: inconsistentUsage },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "inconsistent totals" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(inconsistentTotals)?.usageTelemetryStatus, "invalid");
      assert.deepEqual((await inconsistentTotals.json() as { usage?: unknown }).usage, inconsistentUsage);

      const incomplete = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.incomplete",
                response: {
                  model: DEFAULT_TEST_MODEL,
                  status: "incomplete",
                  usage: {
                    input_tokens: 100,
                    input_tokens_details: { cached_tokens: 80, cache_write_tokens: 0 },
                    output_tokens: 0,
                    total_tokens: 100,
                  },
                },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "incomplete" }),
            }),
          ),
      );
      assert.equal(incomplete.status, 200);
      assert.deepEqual(getResponseTelemetry(incomplete), {
        ...getResponseTelemetry(incomplete),
        completed: false,
        inputTokens: 100,
        cachedInputTokens: 80,
        cacheWriteInputTokens: 0,
        outputTokens: 0,
        totalTokens: 100,
        usageObserved: true,
        usageTelemetryStatus: "reported",
        streamTerminalType: "response.incomplete",
      });

      const malformedUsage = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: { model: DEFAULT_TEST_MODEL, output: [], usage: null },
              })
            }\n\n`,
          ]),
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "malformed usage" }),
            }),
          ),
      );
      assert.equal(getResponseTelemetry(malformedUsage)?.usageObserved, true);
      assert.equal(getResponseTelemetry(malformedUsage)?.usageTelemetryStatus, "invalid");
    },
  );
});

Deno.test("openai: preserves standard explicit cache breakpoints without aliases", async (t) => {
  await t.step("Responses preserves each supported input content block", async () => {
    let recordedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              prompt_cache_options: { mode: "explicit" },
              input: [{
                type: "message",
                role: "user",
                content: [
                  { type: "input_text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } },
                  {
                    type: "input_image",
                    image_url: "https://example.test/stable.png",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  },
                  {
                    type: "input_file",
                    file_id: "file_stable",
                    detail: "high",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  },
                ],
              }],
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as unknown as Record<string, unknown>;
    const content = ((recorded.input as Array<Record<string, unknown>>)[0]?.content ?? []) as Array<
      Record<string, unknown>
    >;
    assert.deepEqual(
      content.map((item) => item.prompt_cache_breakpoint),
      [{ mode: "explicit" }, { mode: "explicit" }, { mode: "explicit" }],
    );
    assert.deepEqual(content[2], {
      type: "input_file",
      file_id: "file_stable",
      detail: "high",
      prompt_cache_breakpoint: { mode: "explicit" },
    });
  });

  await t.step("Responses preserves function-call output content breakpoints and file detail", async () => {
    let recordedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              prompt_cache_options: { mode: "explicit" },
              input: [{
                type: "function_call_output",
                call_id: "call_cache_result",
                output: [
                  { type: "input_text", text: "stable tool result", prompt_cache_breakpoint: { mode: "explicit" } },
                  {
                    type: "input_image",
                    image_url: "https://example.test/tool-result.png",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  },
                  {
                    type: "input_file",
                    file_id: "file_tool_result",
                    detail: "low",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  },
                ],
              }],
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const input = (recordedBody as unknown as Record<string, unknown>).input as Array<Record<string, unknown>>;
    assert.deepEqual(input[0]?.output, [
      { type: "input_text", text: "stable tool result", prompt_cache_breakpoint: { mode: "explicit" } },
      {
        type: "input_image",
        image_url: "https://example.test/tool-result.png",
        prompt_cache_breakpoint: { mode: "explicit" },
      },
      {
        type: "input_file",
        file_id: "file_tool_result",
        detail: "low",
        prompt_cache_breakpoint: { mode: "explicit" },
      },
    ]);
    assert.equal(getResponseTelemetry(response)?.explicitBreakpointCount, 3);
  });

  await t.step(
    "Chat preserves text/image and moves breakpoint-bearing instructions into ordered developer input",
    async () => {
      let recordedBody: Record<string, unknown> | null = null;
      const response = await withFetchMock(
        (_url, bodyText) => {
          recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                prompt_cache_key: "stable-support-prefix",
                prompt_cache_options: { mode: "explicit" },
                messages: [
                  {
                    role: "system",
                    content: [{ type: "text", text: "stable system", prompt_cache_breakpoint: { mode: "explicit" } }],
                  },
                  { role: "developer", content: [{ type: "text", text: "second stable instruction" }] },
                  {
                    role: "user",
                    content: [
                      { type: "text", text: "question", prompt_cache_breakpoint: { mode: "explicit" } },
                      {
                        type: "image_url",
                        image_url: { url: "https://example.test/question.png" },
                        prompt_cache_breakpoint: { mode: "explicit" },
                      },
                    ],
                  },
                ],
              }),
            }),
          ),
      );
      assert.equal(response.status, 200);
      assert.ok(recordedBody);
      const recorded = recordedBody as unknown as Record<string, unknown>;
      assert.equal("instructions" in recorded, false);
      const input = recorded.input as Array<Record<string, unknown>>;
      assert.deepEqual(input.map((item) => item.role), ["developer", "developer", "user"]);
      const first = input[0]?.content as Array<Record<string, unknown>>;
      const last = input[2]?.content as Array<Record<string, unknown>>;
      assert.deepEqual(first[0]?.prompt_cache_breakpoint, { mode: "explicit" });
      assert.deepEqual(last.map((item) => item.prompt_cache_breakpoint), [{ mode: "explicit" }, { mode: "explicit" }]);
      assert.equal(getResponseTelemetry(response)?.explicitBreakpointCount, 3);
      assert.equal(getResponseTelemetry(response)?.promptCacheKeyPresent, true);
      assert.equal(getResponseTelemetry(response)?.promptCacheMode, "explicit");
    },
  );

  await t.step("Chat rejects a breakpoint on assistant output content", async () => {
    const response = await handleChatCompletions(
      new Request("https://ai.ubq.fi/v1/chat/completions", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          model: DEFAULT_TEST_MODEL,
          messages: [
            {
              role: "assistant",
              content: [{ type: "text", text: "prior reply", prompt_cache_breakpoint: { mode: "explicit" } }],
            },
            { role: "user", content: "continue" },
          ],
        }),
      }),
    );
    assert.equal(response.status, 400);
    const body = await response.json() as { error?: { param?: string } };
    assert.equal(body.error?.param, "messages[0].content[0].prompt_cache_breakpoint");
  });

  await t.step("Chat preserves a tool-output breakpoint through function_call_output", async () => {
    let dispatches = 0;
    let recordedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (_url, bodyText) => {
        dispatches += 1;
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              messages: [{
                role: "tool",
                tool_call_id: "call_stable_tool_output",
                content: [{
                  type: "text",
                  text: "stable tool result",
                  prompt_cache_breakpoint: { mode: "explicit" },
                }],
              }],
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.equal(dispatches, 1);
    assert.ok(recordedBody);
    const input = (recordedBody as unknown as Record<string, unknown>).input as Array<Record<string, unknown>>;
    assert.deepEqual(input[0]?.output, [
      { type: "input_text", text: "stable tool result", prompt_cache_breakpoint: { mode: "explicit" } },
    ]);
    assert.equal(getResponseTelemetry(response)?.explicitBreakpointCount, 1);
  });

  await t.step("Chat maps native file parts and preserves an interleaved developer prefix", async () => {
    let recordedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (_url, bodyText) => {
        recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              prompt_cache_options: { mode: "explicit" },
              messages: [
                {
                  role: "system",
                  content: [{ type: "text", text: "stable system", prompt_cache_breakpoint: { mode: "explicit" } }],
                },
                { role: "user", content: "first question" },
                { role: "developer", content: [{ type: "text", text: "stable developer" }] },
                {
                  role: "user",
                  content: [
                    { type: "text", text: "Read these files", prompt_cache_breakpoint: { mode: "explicit" } },
                    {
                      type: "file",
                      file: { file_id: "file_stable", filename: "stable.txt" },
                      prompt_cache_breakpoint: { mode: "explicit" },
                    },
                    {
                      type: "file",
                      file: { file_data: "data:text/plain;base64,c3RhYmxl", filename: "inline.txt" },
                      prompt_cache_breakpoint: { mode: "explicit" },
                    },
                  ],
                },
              ],
            }),
          }),
        ),
    );
    assert.equal(response.status, 200);
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal("instructions" in recorded, false);
    const input = recorded.input as Array<Record<string, unknown>>;
    assert.deepEqual(input.map((item) => item.role), ["developer", "user", "developer", "user"]);
    assert.deepEqual(input[0]?.content, [
      { type: "input_text", text: "stable system", prompt_cache_breakpoint: { mode: "explicit" } },
    ]);
    assert.deepEqual(input[2]?.content, [{ type: "input_text", text: "stable developer" }]);
    assert.deepEqual(input[3]?.content, [
      { type: "input_text", text: "Read these files", prompt_cache_breakpoint: { mode: "explicit" } },
      {
        type: "input_file",
        file_id: "file_stable",
        filename: "stable.txt",
        prompt_cache_breakpoint: { mode: "explicit" },
      },
      {
        type: "input_file",
        file_data: "data:text/plain;base64,c3RhYmxl",
        filename: "inline.txt",
        prompt_cache_breakpoint: { mode: "explicit" },
      },
    ]);
    assert.equal(getResponseTelemetry(response)?.explicitBreakpointCount, 4);
  });
});

Deno.test("openai: identical cacheable Chat requests render byte-identical upstream bodies", async () => {
  const bodies: string[] = [];
  const requestBody = {
    model: DEFAULT_TEST_MODEL,
    prompt_cache_key: "stable-cache-prefix",
    prompt_cache_options: { mode: "explicit", ttl: "30m" },
    messages: [
      {
        role: "system",
        content: [{ type: "text", text: "Stable instructions", prompt_cache_breakpoint: { mode: "explicit" } }],
      },
      { role: "user", content: [{ type: "text", text: "Variable question" }] },
    ],
  };

  await withFetchMock(
    (_url, bodyText) => {
      assert.ok(bodyText);
      bodies.push(bodyText);
      return sseResponse(baseSseChunks());
    },
    async () => {
      for (let attempt = 0; attempt < 2; attempt += 1) {
        const response = await handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(requestBody),
          }),
        );
        assert.equal(response.status, 200);
      }
    },
  );

  assert.deepEqual(bodies, [bodies[0], bodies[0]]);
  assert.doesNotMatch(bodies[0]!, /"(?:account_id|conversation_id|request_id|timestamp)"/);
});

Deno.test("openai: known-unsupported prompt caching rejects controls and breakpoints before dispatch", async (t) => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const runtimeConfigKey = keyToString(["uos_ai", "runtime_config", "v2"]);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousRuntimeConfig = kvStore.get(runtimeConfigKey);
  kvStore.set(snapshotKey, {
    source: "chatgpt_codex",
    client_version: "0.125.0",
    updated_at_ms: Date.now(),
    models: [{
      slug: DEFAULT_TEST_MODEL,
      supported_reasoning_levels: ["none", "medium"],
      prompt_cache: false,
    }],
  });

  const cases = [
    {
      route: "responses",
      body: { input: "ping", prompt_cache_options: { mode: "implicit" } },
      param: "prompt_cache_options",
    },
    {
      route: "responses",
      body: {
        input: [{ type: "input_text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
      },
      param: "input[0].prompt_cache_breakpoint",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "user", content: "ping" }],
        prompt_cache_key: "stable-prefix",
      },
      param: "prompt_cache_key",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "user", content: "ping" }],
        prompt_cache_retention: "24h",
      },
      param: "prompt_cache_retention",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "user",
          content: [{ type: "text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
        }],
      },
      param: "messages[0].content[0].prompt_cache_breakpoint",
    },
    {
      route: "responses",
      body: {
        input: [{
          type: "function_call_output",
          call_id: "call_cache_result",
          output: [{
            type: "input_text",
            text: "stable tool result",
            prompt_cache_breakpoint: { mode: "explicit" },
          }],
        }],
      },
      param: "input[0].output[0].prompt_cache_breakpoint",
    },
  ] as const;

  try {
    for (const testCase of cases) {
      await t.step(`${testCase.route}/${testCase.param}`, async () => {
        let dispatches = 0;
        const response = await withFetchMock(
          () => {
            dispatches += 1;
            return sseResponse(baseSseChunks());
          },
          () =>
            testCase.route === "chat.completions"
              ? handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
                }),
              )
              : handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
                }),
              ),
        );
        assert.equal(response.status, 400);
        assert.equal(dispatches, 0);
        const payload = await response.json() as { error?: { message?: string; type?: string; param?: string } };
        assert.equal(payload.error?.message, `Prompt caching is not supported for model '${DEFAULT_TEST_MODEL}'.`);
        assert.equal(payload.error?.type, "invalid_request_error");
        assert.equal(payload.error?.param, testCase.param);
      });
    }

    await t.step("omitted metadata remains unknown and forwards standard controls", async () => {
      kvStore.set(snapshotKey, {
        source: "chatgpt_codex",
        client_version: "0.125.0",
        updated_at_ms: Date.now(),
        models: [{ slug: DEFAULT_TEST_MODEL, supported_reasoning_levels: ["none", "medium"] }],
      });

      let dispatches = 0;
      const response = await withFetchMock(
        () => {
          dispatches += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                input: "ping",
                prompt_cache_options: { mode: "implicit" },
              }),
            }),
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(dispatches, 1);
    });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousRuntimeConfig === undefined) kvStore.delete(runtimeConfigKey);
    else kvStore.set(runtimeConfigKey, previousRuntimeConfig);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: active provider cache capabilities reject only known unsupported controls", async (t) => {
  const snapshotKey = keyToString(TEST_CODEX_MODELS_KEY);
  const runtimeConfigKey = keyToString(["uos_ai", "runtime_config", "v2"]);
  const previousSnapshot = kvStore.get(snapshotKey);
  const previousRuntimeConfig = kvStore.get(runtimeConfigKey);
  const controls = (fields: Record<string, unknown>) => ({
    ...fields,
    source: "catalog",
    verified_at_ms: 1_000,
  });
  const setPromptCacheCapabilities = (promptCache: unknown) => {
    kvStore.set(snapshotKey, {
      source: "chatgpt_codex",
      client_version: "0.125.0",
      updated_at_ms: Date.now(),
      models: [{
        slug: DEFAULT_TEST_MODEL,
        supported_reasoning_levels: ["none", "medium"],
        prompt_cache: promptCache,
      }],
    });
  };
  const cases = [
    {
      route: "responses",
      controls: controls({ key: false }),
      body: { input: "ping", prompt_cache_key: "stable-prefix" },
      param: "prompt_cache_key",
    },
    {
      route: "chat.completions",
      controls: controls({ key: false }),
      body: { messages: [{ role: "user", content: "ping" }], prompt_cache_key: "stable-prefix" },
      param: "prompt_cache_key",
    },
    {
      route: "responses",
      controls: controls({ implicit: false }),
      body: { input: "ping", prompt_cache_options: { ttl: "30m" } },
      param: "prompt_cache_options",
    },
    {
      route: "chat.completions",
      controls: controls({ modes: ["explicit"] }),
      body: {
        messages: [{ role: "user", content: "ping" }],
        prompt_cache_options: { mode: "implicit" },
      },
      param: "prompt_cache_options.mode",
    },
    {
      route: "responses",
      controls: controls({ explicit_breakpoints: false }),
      body: {
        input: [{ type: "input_text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
        prompt_cache_options: { mode: "explicit" },
      },
      param: "input[0].prompt_cache_breakpoint",
    },
    {
      route: "chat.completions",
      controls: controls({ modes: ["implicit"] }),
      body: {
        messages: [{
          role: "user",
          content: [{ type: "text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
        }],
        prompt_cache_options: { mode: "explicit" },
      },
      param: "messages[0].content[0].prompt_cache_breakpoint",
    },
    {
      route: "responses",
      controls: controls({ explicit_breakpoints: false }),
      body: { input: "ping", prompt_cache_options: { mode: "explicit" } },
      param: "prompt_cache_options.mode",
    },
    {
      route: "responses",
      controls: controls({ legacy_retentions: ["in_memory"] }),
      body: { input: "ping", prompt_cache_retention: "24h" },
      param: "prompt_cache_retention",
    },
    {
      route: "chat.completions",
      controls: controls({ legacy_retentions: ["in_memory"] }),
      body: { messages: [{ role: "user", content: "ping" }], prompt_cache_retention: "24h" },
      param: "prompt_cache_retention",
    },
    {
      route: "responses",
      controls: controls({ breakpoint_block_types: { responses: ["input_text"] } }),
      body: {
        input: [{
          type: "input_image",
          image_url: "https://example.test/stable.png",
          prompt_cache_breakpoint: { mode: "explicit" },
        }],
      },
      param: "input[0].prompt_cache_breakpoint",
    },
    {
      route: "responses",
      controls: controls({ breakpoint_block_types: { responses: ["input_text"] } }),
      body: {
        input: [{
          type: "message",
          role: "user",
          content: [
            { type: "input_text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } },
            {
              type: "input_image",
              image_url: "https://example.test/later-unsupported.png",
              prompt_cache_breakpoint: { mode: "explicit" },
            },
          ],
        }],
      },
      param: "input[0].content[1].prompt_cache_breakpoint",
    },
    {
      route: "chat.completions",
      controls: controls({ breakpoint_block_types: { chat_completions: ["text"] } }),
      body: {
        messages: [{
          role: "user",
          content: [{
            type: "image_url",
            image_url: { url: "https://example.test/stable.png" },
            prompt_cache_breakpoint: { mode: "explicit" },
          }],
        }],
      },
      param: "messages[0].content[0].prompt_cache_breakpoint",
    },
  ] as const;

  try {
    for (const testCase of cases) {
      await t.step(`${testCase.route}/${testCase.param}`, async () => {
        setPromptCacheCapabilities({
          version: 1,
          providers: [{ id: "codex_chatgpt", controls: testCase.controls }],
        });

        let dispatches = 0;
        const response = await withFetchMock(
          () => {
            dispatches += 1;
            return sseResponse(baseSseChunks());
          },
          () =>
            testCase.route === "chat.completions"
              ? handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
                }),
              )
              : handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
                }),
              ),
        );
        assert.equal(response.status, 400);
        assert.equal(dispatches, 0);
        const payload = await response.json() as { error?: { message?: string; type?: string; param?: string } };
        assert.equal(
          payload.error?.message,
          `Prompt cache control '${testCase.param}' is not supported for model '${DEFAULT_TEST_MODEL}'.`,
        );
        assert.equal(payload.error?.type, "invalid_request_error");
        assert.equal(payload.error?.param, testCase.param);
      });
    }

    await t.step(
      "unsupported catalog TTL metadata remains unknown instead of rejecting the public 30m value",
      async () => {
        setPromptCacheCapabilities({
          version: 1,
          providers: [{ id: "codex_chatgpt", controls: controls({ ttls: ["5m"] }) }],
        });

        let dispatches = 0;
        const response = await withFetchMock(
          () => {
            dispatches += 1;
            return sseResponse(baseSseChunks());
          },
          () =>
            handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  input: "ping",
                  prompt_cache_options: { ttl: "30m" },
                }),
              }),
            ),
        );
        assert.equal(response.status, 200);
        assert.equal(dispatches, 1);
      },
    );

    await t.step("omitted active-provider fields and other providers remain unknown", async () => {
      setPromptCacheCapabilities({
        version: 1,
        providers: [
          { id: "codex_chatgpt", controls: controls({}) },
          { id: "yunwu", controls: controls({ key: false, explicit_breakpoints: false }) },
        ],
      });

      let dispatches = 0;
      const response = await withFetchMock(
        () => {
          dispatches += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                prompt_cache_key: "stable-prefix",
                prompt_cache_options: { mode: "explicit", ttl: "30m" },
                input: [{ type: "input_text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
              }),
            }),
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(dispatches, 1);
    });

    await t.step("malformed capability metadata remains unknown", async () => {
      setPromptCacheCapabilities({ version: 1, providers: [] });

      let dispatches = 0;
      const response = await withFetchMock(
        () => {
          dispatches += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                prompt_cache_key: "stable-prefix",
                prompt_cache_options: { mode: "explicit", ttl: "30m" },
                messages: [{
                  role: "user",
                  content: [{ type: "text", text: "stable", prompt_cache_breakpoint: { mode: "explicit" } }],
                }],
              }),
            }),
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(dispatches, 1);
    });
  } finally {
    if (previousSnapshot === undefined) kvStore.delete(snapshotKey);
    else kvStore.set(snapshotKey, previousSnapshot);
    if (previousRuntimeConfig === undefined) kvStore.delete(runtimeConfigKey);
    else kvStore.set(runtimeConfigKey, previousRuntimeConfig);
    resetRuntimeConfigCacheForTest();
  }
});

Deno.test("openai: rejects lossy Chat cache breakpoint content before dispatch", async (t) => {
  const cases = [
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "system",
          content: [{ type: "image_url", image_url: { url: "https://example.test/a.png" } }],
        }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "developer", content: [{ type: "file", file: { file_id: "file_stable" } }] }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "developer",
          content: [{ type: "input_audio", input_audio: { data: "abc", format: "wav" } }],
        }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "user", content: [{ type: "input_image", image_url: "https://example.test/a.png" }] }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "user",
          content: [{ type: "image_url", image_url: { url: "https://example.test/a.png" }, detail: "high" }],
        }],
      },
      param: "messages[0].content[0].detail",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "user", content: [{ type: "text", text: "stable", unexpected: true }] }],
      },
      param: "messages[0].content[0].unexpected",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "user",
          content: [{ type: "image_url", image_url: { url: "https://example.test/a.png", unexpected: true } }],
        }],
      },
      param: "messages[0].content[0].image_url.unexpected",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "assistant", content: [{ type: "refusal", refusal: "No", unexpected: true }] }],
      },
      param: "messages[0].content[0].unexpected",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "user",
          content: "stable",
          prompt_cache_breakpoint: { mode: "explicit" },
        }],
      },
      param: "messages[0].prompt_cache_breakpoint",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "assistant",
          content: [{ type: "refusal", refusal: "No", prompt_cache_breakpoint: { mode: "explicit" } }],
        }],
      },
      param: "messages[0].content[0].prompt_cache_breakpoint",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "user",
          content: [{
            type: "input_audio",
            input_audio: { data: "abc", format: "wav" },
            prompt_cache_breakpoint: { mode: "explicit" },
          }],
        }],
      },
      param: "messages[0].content[0].prompt_cache_breakpoint",
    },
    {
      route: "responses",
      body: {
        input: [{
          type: "message",
          role: "user",
          content: "stable",
          prompt_cache_breakpoint: { mode: "explicit" },
        }],
      },
      param: "input[0].prompt_cache_breakpoint",
    },
    {
      route: "responses",
      body: {
        input: [{
          type: "function_call_output",
          call_id: "call_stable",
          output: "stable",
          prompt_cache_breakpoint: { mode: "explicit" },
        }],
      },
      param: "input[0].prompt_cache_breakpoint",
    },
  ] as const;

  for (const testCase of cases) {
    await t.step(`${testCase.route}/${testCase.param}`, async () => {
      let dispatches = 0;
      const response = await withFetchMock(
        () => {
          dispatches += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          testCase.route === "chat.completions"
            ? handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            )
            : handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            ),
      );
      assert.equal(response.status, 400);
      assert.equal(dispatches, 0);
      const payload = await response.json() as { error?: { param?: string; type?: string } };
      assert.equal(payload.error?.type, "invalid_request_error");
      assert.equal(payload.error?.param, testCase.param);
    });
  }
});

Deno.test("openai: Responses preserves an explicit empty instructions string", async () => {
  let recordedBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (_url, bodyText) => {
      recordedBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      return sseResponse(baseSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", instructions: "" }),
        }),
      ),
  );
  assert.equal(response.status, 200);
  assert.ok(recordedBody);
  assert.equal((recordedBody as Record<string, unknown>).instructions, "");
});

Deno.test("openai: strict request fields reject malformed values without dispatch", async (t) => {
  const cases = [
    {
      route: "responses",
      body: { input: "ping", stream: null },
      param: "stream",
    },
    {
      route: "chat.completions",
      body: { messages: [{ role: "user", content: "ping" }], stream: "true" },
      param: "stream",
    },
    {
      route: "responses",
      body: { input: "ping", reasoning: [] },
      param: "reasoning",
    },
    {
      route: "responses",
      body: { input: [{ role: "user", content: [{ type: "input_image", image_url: "x", unexpected: true }] }] },
      param: "input[0].content[0].unexpected",
    },
    {
      route: "chat.completions",
      body: { messages: [{ role: "tool", tool_call_id: "call", content: "result", tool_calls: [] }] },
      param: "messages[0].tool_calls",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "tool", tool_call_id: "call", content: [{ type: "input_text", text: "result" }] }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{ role: "tool", tool_call_id: "call", content: [{ type: "output_text", text: "result" }] }],
      },
      param: "messages[0].content[0].type",
    },
    {
      route: "responses",
      body: { input: [{ type: "message", role: "tool", content: "result" }] },
      param: "input[0].role",
    },
    {
      route: "responses",
      body: { input: [{ type: "message", role: "user", content: [{ type: "output_text", text: "result" }] }] },
      param: "input[0].content[0].type",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "assistant",
          content: null,
          tool_calls: [{
            id: "call",
            type: "function",
            function: { name: "tool", arguments: "{}" },
            unexpected: true,
          }],
        }],
      },
      param: "messages[0].tool_calls[0].unexpected",
    },
    {
      route: "chat.completions",
      body: {
        messages: [{
          role: "assistant",
          content: null,
          tool_calls: [{
            id: "call",
            type: "function",
            function: { name: "tool", arguments: "{}", unexpected: true },
          }],
        }],
      },
      param: "messages[0].tool_calls[0].function.unexpected",
    },
  ] as const;
  for (const testCase of cases) {
    await t.step(`${testCase.route}/${testCase.param}`, async () => {
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          testCase.route === "responses"
            ? handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            )
            : handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            ),
      );
      assert.equal(response.status, 400);
      assert.equal(calls, 0);
      const payload = await response.json() as { error?: { param?: string } };
      assert.equal(payload.error?.param, testCase.param);
    });
  }
  await t.step("input[0].type", async () => {
    let dispatches = 0;
    const response = await withFetchMock(
      () => {
        dispatches += 1;
        return sseResponse(baseSseChunks());
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: [{ type: "text", text: "Chat-only alias" }] }),
          }),
        ),
    );
    assert.equal(response.status, 400);
    assert.equal(dispatches, 0);
    const payload = await response.json() as { error?: { param?: string } };
    assert.equal(payload.error?.param, "input[0].type");
  });
});

Deno.test("openai: validates standard prompt-cache controls before dispatch", async (t) => {
  const cases = [
    {
      route: "responses",
      body: { input: "ping", prompt_cache_key: 1 },
      param: "prompt_cache_key",
    },
    {
      route: "chat.completions",
      body: { messages: [{ role: "user", content: "ping" }], prompt_cache_options: null },
      param: "prompt_cache_options",
    },
    {
      route: "responses",
      body: { input: "ping", prompt_cache_options: { mode: "automatic" } },
      param: "prompt_cache_options.mode",
    },
    {
      route: "chat.completions",
      body: { messages: [{ role: "user", content: "ping" }], prompt_cache_options: { ttl: "24h" } },
      param: "prompt_cache_options.ttl",
    },
    {
      route: "responses",
      body: { input: "ping", prompt_cache_options: { mode: "implicit", unexpected: true } },
      param: "prompt_cache_options.unexpected",
    },
    {
      route: "chat.completions",
      body: { messages: [{ role: "user", content: "ping" }], prompt_cache_retention: "forever" },
      param: "prompt_cache_retention",
    },
  ] as const;

  for (const testCase of cases) {
    await t.step(`${testCase.route}/${testCase.param}`, async () => {
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          testCase.route === "responses"
            ? handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            )
            : handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            ),
      );
      assert.equal(response.status, 400);
      assert.equal(calls, 0);
      const payload = await response.json() as { error?: { param?: string } };
      assert.equal(payload.error?.param, testCase.param);
    });
  }
});

Deno.test("openai: rejects gateway-only cache aliases before dispatch", async (t) => {
  const cases = [
    { route: "responses", body: { input: "ping", cache_key: "not-standard" }, field: "cache_key" },
    {
      route: "chat.completions",
      body: { messages: [{ role: "user", content: "ping" }], cache_affinity: "not-standard" },
      field: "cache_affinity",
    },
  ] as const;

  for (const testCase of cases) {
    await t.step(`${testCase.route}/${testCase.field}`, async () => {
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          testCase.route === "responses"
            ? handleResponses(
              new Request("https://ai.ubq.fi/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            )
            : handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, ...testCase.body }),
              }),
            ),
      );
      assert.equal(response.status, 400);
      assert.equal(calls, 0);
      const payload = await response.json() as { error?: { message?: string } };
      assert.match(payload.error?.message ?? "", new RegExp(testCase.field));
    });
  }
});

Deno.test("openai: both endpoints reject every non-boolean stream shape before dispatch", async (t) => {
  const invalidValues: Array<readonly [string, unknown]> = [
    ["null", null],
    ["string", "true"],
    ["number", 1],
    ["array", []],
    ["object", {}],
  ];
  for (const route of ["chat.completions", "responses"] as const) {
    for (const [label, stream] of invalidValues) {
      await t.step(`${route}/${label}`, async () => {
        let dispatches = 0;
        const response = await withFetchMock(
          () => {
            dispatches += 1;
            return sseResponse(baseSseChunks());
          },
          () =>
            route === "chat.completions"
              ? handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    stream,
                    messages: [{ role: "user", content: "ping" }],
                  }),
                }),
              )
              : handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, stream, input: "ping" }),
                }),
              ),
        );
        assert.equal(response.status, 400);
        assert.equal(dispatches, 0);
        const payload = await response.json() as { error?: { param?: string } };
        assert.equal(payload.error?.param, "stream");
      });
    }
  }
});

Deno.test("openai: native Responses reject malformed known content fields and unsupported content types", async (t) => {
  const cases = [
    {
      content: [{ type: "input_image", image_url: "https://example.test/image", file_id: "file_image" }],
      param: "input[0].content[0].image_url",
    },
    {
      content: [{ type: "input_image", detail: "low" }],
      param: "input[0].content[0].image_url",
    },
    {
      content: [{ type: "input_file", file_id: 3 }],
      param: "input[0].content[0].file_id",
    },
    {
      content: [{ type: "input_file", filename: "missing-source.txt" }],
      param: "input[0].content[0].file_id",
    },
    {
      content: [{ type: "input_audio", data: "ignored" }],
      param: "input[0].content[0].type",
    },
    {
      content: [{ type: "text", text: "Chat-only alias" }],
      param: "input[0].content[0].type",
    },
    {
      content: [{ type: "image_url", image_url: "https://example.test/image" }],
      param: "input[0].content[0].type",
    },
    {
      content: [{ type: "input_text" }],
      param: "input[0].content[0].text",
    },
  ] as const;
  for (const testCase of cases) {
    await t.step(testCase.param, async () => {
      let dispatches = 0;
      const response = await withFetchMock(
        () => {
          dispatches += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                input: [{ type: "message", role: "user", content: testCase.content }],
              }),
            }),
          ),
      );
      assert.equal(response.status, 400);
      assert.equal(dispatches, 0);
      const payload = await response.json() as { error?: { param?: string } };
      assert.equal(payload.error?.param, testCase.param);
    });
  }
});

Deno.test("openai: buffered Chat Completions release the upstream stream reader", async () => {
  let upstreamBody: ReadableStream<Uint8Array> | null = null;
  const response = await withFetchMock(
    () => {
      upstreamBody = new ReadableStream<Uint8Array>({
        start(controller) {
          for (const chunk of baseSseChunks()) controller.enqueue(TEXT_ENCODER.encode(chunk));
          controller.close();
        },
      });
      return new Response(upstreamBody, {
        status: 200,
        headers: { "Content-Type": "text/event-stream" },
      });
    },
    () =>
      handleChatCompletions(
        new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            messages: [{ role: "user", content: "ping" }],
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  await response.text();
  const body = upstreamBody as ReadableStream<Uint8Array> | null;
  assert.ok(body);
  assert.equal(body.locked, false);
});

Deno.test("openai: streamed Responses force the SSE content type", async () => {
  const response = await withFetchMock(
    () =>
      new Response(sseResponse(baseSseChunks()).body, {
        status: 200,
        // Deliberately omit Content-Type to model a compatible upstream that
        // returns valid SSE bytes with an incomplete header set.
      }),
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            input: "ping",
            stream: true,
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("Content-Type"), "text/event-stream");
  assert.match(await response.text(), /response.completed/);
});

Deno.test("auth: kernel attestation tokens are reusable within TTL", async () => {
  const keyPair = await crypto.subtle.generateKey(
    {
      name: "RSASSA-PKCS1-v1_5",
      modulusLength: 2048,
      publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      hash: "SHA-256",
    },
    true,
    ["sign", "verify"],
  );

  const spki = new Uint8Array(await crypto.subtle.exportKey("spki", keyPair.publicKey));
  const publicPem = toPublicKeyPem(spki);
  kvStore.set(keyToString(["uos_ai", "kernel_pubkeys"]), [{ pem: publicPem }]);

  const bearerToken = "ghs_test_token";
  const nowSeconds = Math.floor(Date.now() / 1000);
  const payload = {
    iss: "ubiquity-os-kernel",
    aud: "ai.ubq.fi",
    iat: nowSeconds,
    exp: nowSeconds + 600,
    jti: `jti_${crypto.randomUUID()}`,
    owner: "acme",
    repo: "demo",
    installation_id: null,
    auth_token_sha256: await sha256Base64Url(bearerToken),
    state_id: "state_test",
  };

  const header = { alg: "RS256", typ: "JWT" };
  const headerB64 = encodeJsonBase64Url(header);
  const payloadB64 = encodeJsonBase64Url(payload);
  const signingInput = `${headerB64}.${payloadB64}`;
  const signature = new Uint8Array(
    await crypto.subtle.sign("RSASSA-PKCS1-v1_5", keyPair.privateKey, TEXT_ENCODER.encode(signingInput)),
  );
  const kernelToken = `${signingInput}.${encodeBase64Url(signature)}`;

  const { getKernelAttestationContext } = await import("../src/auth.ts");

  const req = new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "X-Ubiquity-Kernel-Token": kernelToken },
    body: "{}",
  });

  const first = await getKernelAttestationContext(req, bearerToken);
  const second = await getKernelAttestationContext(req, bearerToken);
  assert.ok(first);
  assert.ok(second);
  assert.deepEqual(second, first);
});

addEventListener("unload", () => {
  (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
});
