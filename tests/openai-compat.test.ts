import assert from "node:assert/strict";
import type { CodexBankedResetConfig } from "../src/codex_banked_reset.ts";
import type { CodexUsageResetProvider } from "../src/codex_banked_reset_provider.ts";
import type { CodexAuthPoolState } from "../src/types.ts";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { setStreamFirstEventDeadlineMsForTest } from "../src/inference_deadline.ts";
import { RELEASE_GIT_SHA } from "../src/release.ts";
import { MAX_RESPONSES_SSE_EVENT_BYTES } from "../src/responses_stream.ts";
import { sha256Base64Url, sha256Hex } from "../src/utils.ts";
import { readPromptCacheAnalytics, recordPromptCacheAnalytics } from "../src/prompt_cache_analytics.ts";
import { CountingKv } from "./helpers/counting_kv.ts";
import { setKvForTest } from "../src/kv.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
const TERRA_TEST_MODEL = "gpt-5.6-terra";
const TEMPORARY_FREE_SURPLUS_TEST_MODEL = "glm-5.2";
const TEST_CODEX_MODELS_KEY = ["ubq_ai", "codex_models"] as const;

const kvStore = new Map<string, unknown>();
type OpenAiAtomicOp = { type: "set" | "delete"; key: Deno.KvKey; value?: unknown };
let atomicCommitFailure: ((ops: readonly OpenAiAtomicOp[]) => Error | null) | null = null;
const atomicCommitObservation: {
  observer: ((ops: readonly OpenAiAtomicOp[]) => void) | null;
} = { observer: null };
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
  }, {
    slug: TERRA_TEST_MODEL,
    display_name: "GPT-5.6 Terra fixture",
    context_window: 272000,
    max_context_window: 1000000,
    auto_compact_token_limit: null,
    default_reasoning_level: "medium",
    supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh", "max", "ultra"],
    reasoning_effort_wire_map: { ultra: "max" },
  }],
});
kvStore.set(keyToString(["uos_ai", "voyage_api_key"]), "voyage_test_key");

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(
      ({
        key,
        value: kvStore.get(keyToString(key)) ?? null,
        versionstamp: kvStore.has(keyToString(key)) ? "00000000000000000001" : null,
      }) as Deno.KvEntryMaybe<unknown>,
    ),
  getMany: (keys: readonly Deno.KvKey[]) =>
    Promise.resolve(keys.map((key) => ({
      key,
      value: kvStore.get(keyToString(key)) ?? null,
      versionstamp: kvStore.has(keyToString(key)) ? "00000000000000000001" : null,
    }))),
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
        atomicCommitObservation.observer?.(ops.slice());
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

setKvForTest(kvStub);

const {
  extractUsageTokens,
  getResponseTelemetry,
  handleChatCompletions,
  handleModelCapabilities,
  handlePublicModelCatalog,
  handleModels,
  handleResponses,
  setCodexBankedResetOptionsForTest,
} = await import("../src/openai.ts");
const {
  fetchMeteredModels,
  METERED_MODELS_CACHE_TTL_MS,
  resetMeteredModelsCacheForTest,
  setMeteredModelsFetchForTest,
} = await import("../src/metered.ts");
const {
  fetchSurplusModels,
  resetSurplusModelsCacheForTest,
  SURPLUS_MODELS_CACHE_TTL_MS,
} = await import("../src/surplus.ts");
const { ApiKeyQuotaDispatchError } = await import("../src/api_key_policy.ts");
const { withCors } = await import("../src/http.ts");
const { default: gatewayHandler, withTerminalRequestLog } = await import("../src/handler.ts");
const { resetRuntimeConfigCacheForTest } = await import("../src/runtime_config.ts");
const { DEBUG_ROUTING_KEY, resetDebugRoutingCacheForTest } = await import("../src/debug_routing.ts");
const {
  setRemovedProviderApiKeyForTest,
  setRemovedProviderTestAdapterForTest,
} = await import("../src/removed_provider.ts");
const {
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");
const {
  deriveCodexAccountAffinityIdentity,
  recordCodexAccountAffinity,
} = await import("../src/codex_account_affinity.ts");
const { attemptCodexBankedReset } = await import("../src/codex_banked_reset.ts");
const {
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  getCodexQuotaBlockFence,
  isCodexQuotaBlockFenceCurrent,
  markCodexQuotaBlocked,
  markCodexUpstreamTimeout,
  resetCodexAccountRoutingForTest,
  selectCodexRoutingAccounts,
} = await import("../src/codex_account_routing.ts");
const { projectCerebrasToolSchema, setCerebrasFetchTimeoutMsForTest } = await import("../src/cerebras.ts");
const { recordCodexProviderHealth, resetProviderHealthThrottleForTest } = await import(
  "../src/provider_health.ts"
);

const TEXT_ENCODER = new TextEncoder();
const utf8ByteLength = (value: string): number => TEXT_ENCODER.encode(value).byteLength;

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

const authoritativeCodexQuotaResponse = (headers?: HeadersInit): Response => {
  const responseHeaders = new Headers(headers);
  responseHeaders.set("Content-Type", "application/json");
  responseHeaders.set(
    "Retry-After",
    new Date((Math.floor(Date.now() / 1_000) + 60) * 1_000).toUTCString(),
  );
  return new Response(
    JSON.stringify({ error: { message: "Primary limited", type: "usage_limit_reached" } }),
    { status: 429, headers: responseHeaders },
  );
};

const responsesRequest = (
  body: Record<string, unknown> = {},
  signal?: AbortSignal,
): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true, ...body }),
    signal,
  });

const parseResponsesSseValues = (value: string): Record<string, unknown>[] =>
  [...value.matchAll(/^data: (.+)$/gm)].map((match) => JSON.parse(match[1]!) as Record<string, unknown>);

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
  kvStore.delete(keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY));
  resetCodexAccountRoutingForTest();
  resetCodexAuthCacheForTest();
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
  provider?: string;
  provider_request_id?: string | null;
  reconciliation_attempts?: number;
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
  _options: Readonly<Record<never, never>> = {},
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

const liveBankedResetFixtureConfig = (): CodexBankedResetConfig => ({
  enabled: true,
  mode: "live",
  maxGlobalPerDay: 1,
  maxPerAccountPerWindow: 1,
});

const createVerifiedBankedResetFixture = async (): Promise<readonly string[]> => {
  const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
  const now = Date.now();
  const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, now, DEFAULT_TEST_MODEL);
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
      supportedResetTypes: ["codex_rate_limits"],
    },
    readInventory: () => {
      calls.push("inventory");
      return Promise.resolve({
        availableCount: 1,
        observedAtMs: now,
        credits: [{ id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null }],
      });
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
      config: liveBankedResetFixtureConfig(),
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
  const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, now, DEFAULT_TEST_MODEL);
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
      supportedResetTypes: ["codex_rate_limits"],
    },
    readInventory: () => {
      calls.push("inventory");
      return Promise.resolve({
        availableCount: 1,
        observedAtMs: now,
        credits: [{ id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null }],
      });
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
      config: liveBankedResetFixtureConfig(),
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
            const responseStatus = response.status;
            const responseContentType = response.headers.get("Content-Type");
            const responseUpstream = response.headers.get("x-uos-upstream");
            // A streamed response owns the recovery probe until its terminal
            // event is consumed and validated; merely creating the Response
            // is not proof of successful recovery.
            const responseBody = await response.text();
            const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
            const routingAfterRecovery = await selectCodexRoutingAccounts(
              authPool,
              authPool.accounts,
              Date.now(),
              DEFAULT_TEST_MODEL,
            );
            return {
              responseStatus,
              responseContentType,
              responseUpstream,
              responseBody,
              resetProviderCalls,
              routingAfterRecovery: routingAfterRecovery.kind,
            };
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
      assert.equal(result.responseStatus, 200);
      assert.equal(result.responseUpstream, "chatgpt_codex");
      if (clientWantsStream) {
        assert.equal(result.responseContentType, "text/event-stream");
        assert.match(result.responseBody, new RegExp(postResetText));
        assert.match(result.responseBody, /response\.completed/);
      } else {
        const payload = JSON.parse(result.responseBody) as Record<string, unknown>;
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

Deno.test("openai: legacy timeout circuits do not short-circuit later requests", async () => {
  let inferenceCalls = 0;
  const response = await withFetchMock(
    () => {
      inferenceCalls += 1;
      return sseResponse(baseSseChunks());
    },
    async () => {
      const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
      const selected = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
      assert.equal(selected.kind, "eligible");
      if (selected.kind !== "eligible") throw new Error("expected an eligible timeout fixture account");
      await markCodexUpstreamTimeout(selected.accounts[0]!);
      return await handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "timeout circuit" }),
        }),
      );
    },
  );

  assert.equal(response.status, 200);
  assert.equal(inferenceCalls, 1);
  assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
  await response.text();
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
            supportedResetTypes: ["codex_rate_limits"],
          },
          readInventory: () => {
            providerCalls.push("inventory");
            return Promise.resolve({
              availableCount: 1,
              observedAtMs: Date.now(),
              credits: [
                { id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null },
              ],
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
              config: liveBankedResetFixtureConfig(),
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
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.equal("prompt_cache_options" in recorded, false);
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
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal(recorded["model"], DEFAULT_TEST_MODEL);
    assert.deepEqual(recorded["reasoning"], { effort: "low" });
    assert.equal("temperature" in recorded, false);
    assert.equal("max_output_tokens" in recorded, false);
    assert.equal("moderation" in recorded, false);
    assert.equal("prompt_cache_options" in recorded, false);
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

  await t.step("responses keeps previous_response_id as an explicit ignored warning", async () => {
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
              input: "The full input remains part of this request.",
              previous_response_id: "resp_prior_context_is_not_used",
              stream: true,
            }),
          }),
        ),
    );

    assert.equal(response.status, 200);
    assert.ok(parseWarnings(response.headers.get("x-uos-warning")).includes("previous_response_id_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal("previous_response_id" in recorded, false);
    assert.deepEqual(recorded["input"], [{
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "The full input remains part of this request." }],
    }]);
    await response.text();
  });
});

Deno.test("openai: Responses byte baseline keeps request and stream directions separate", async () => {
  const contextManagement = [{ type: "compaction", compact_threshold: 2000 }];
  const clientRequestBody = JSON.stringify({
    model: DEFAULT_TEST_MODEL,
    input: "ping",
    stream: true,
    context_management: contextManagement,
  });
  const upstreamChunks = baseSseChunks();
  const upstreamStreamBody = upstreamChunks.join("");
  let serializedCodexRequest: string | null = null;

  const response = await withFetchMock(
    (_url, bodyText, init) => {
      const headers = new Headers(init?.headers);
      assert.equal(headers.get("Content-Type"), "application/json");
      assert.equal(headers.get("Content-Encoding"), null);
      assert.ok(bodyText);
      serializedCodexRequest = bodyText;
      return sseResponse(upstreamChunks);
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: clientRequestBody,
        }),
      ),
  );

  assert.equal(response.status, 200);
  const downstreamStreamBody = await response.text();
  assert.ok(serializedCodexRequest);
  assert.deepEqual(JSON.parse(serializedCodexRequest), {
    model: DEFAULT_TEST_MODEL,
    input: [{
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "ping" }],
    }],
    store: false,
    stream: true,
    reasoning: { effort: "low" },
    context_management: contextManagement,
  });
  assert.deepEqual(
    {
      inboundClientRequestBodyBytes: utf8ByteLength(clientRequestBody),
      outboundCodexRequestBodyBytes: utf8ByteLength(serializedCodexRequest),
      inboundCodexStreamBodyBytes: utf8ByteLength(upstreamStreamBody),
      outboundClientStreamBodyBytes: utf8ByteLength(downstreamStreamBody),
    },
    {
      inboundClientRequestBodyBytes: 132,
      outboundCodexRequestBodyBytes: 251,
      inboundCodexStreamBodyBytes: 296,
      outboundClientStreamBodyBytes: 296,
    },
  );
  assert.equal(downstreamStreamBody, upstreamStreamBody);
});

Deno.test("openai: expired Codex auth returns a 503 re-auth warning through Responses", async () => {
  const authKey = keyToString(["ubq_ai", "codex_auth"]);
  const previousAuth = kvStore.get(authKey);
  const now = Date.now();
  let refreshCalls = 0;
  kvStore.set(
    authKey,
    {
      accounts: [{
        access_token: "expired-access-token",
        refresh_token: "expired-refresh-token",
        account_id: "expired-account",
        updated_at_ms: now - 10 * 60_000,
      }],
      updated_at_ms: now - 10 * 60_000,
    } satisfies CodexAuthPoolState,
  );

  try {
    const response = await withFetchMock(
      (url) => {
        if (url === "https://auth.openai.com/oauth/token") {
          refreshCalls += 1;
          return new Response(JSON.stringify({ error: "invalid_grant" }), {
            status: 401,
            headers: { "Content-Type": "application/json" },
          });
        }
        throw new Error(`Inference must not run with expired auth: ${url}`);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
          }),
        ),
    );

    const payload = await response.json() as { error?: { code?: string; message?: string; type?: string } };
    assert.equal(response.status, 503);
    assert.equal(response.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    assert.equal(payload.error?.code, "codex_auth_invalid");
    assert.equal(payload.error?.type, "server_error");
    assert.ok(payload.error?.message?.includes(CODEX_AUTH_REAUTH_MESSAGE));
    assert.match(payload.error?.message ?? "", /upload a fresh auth\.json/i);
    assert.equal(refreshCalls, 1);
  } finally {
    if (previousAuth === undefined) kvStore.delete(authKey);
    else kvStore.set(authKey, previousAuth);
    resetCodexAuthCacheForTest();
  }
});

Deno.test("openai: an expired access token makes a quota-shaped 403 actionable", async () => {
  const authKey = keyToString(["ubq_ai", "codex_auth"]);
  const previousAuth = kvStore.get(authKey);
  const now = Date.now();
  const expiredToken = `${encodeJsonBase64Url({ alg: "none" })}.${
    encodeJsonBase64Url({
      exp: Math.floor((now - 60_000) / 1000),
    })
  }.expired`;
  let inferenceCalls = 0;
  kvStore.set(
    authKey,
    {
      accounts: [{
        access_token: expiredToken,
        refresh_token: "expired-refresh-token",
        account_id: "expired-account",
        updated_at_ms: now,
      }],
      updated_at_ms: now,
    } satisfies CodexAuthPoolState,
  );

  try {
    const response = await withFetchMock(
      (url) => {
        if (url === "https://auth.openai.com/oauth/token") {
          return new Response(
            JSON.stringify({
              access_token: expiredToken,
              refresh_token: "rotated-refresh-token",
            }),
            {
              status: 200,
              headers: { "Content-Type": "application/json" },
            },
          );
        }
        inferenceCalls += 1;
        return new Response(JSON.stringify({ error: { message: "user quota is not enough" } }), {
          status: 403,
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
        ),
    );

    const payload = await response.json() as { error?: { message?: string } };
    assert.equal(response.status, 403);
    assert.equal(response.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    assert.ok(payload.error?.message?.includes(CODEX_AUTH_REAUTH_MESSAGE));
    assert.equal(inferenceCalls, 1);
  } finally {
    if (previousAuth === undefined) kvStore.delete(authKey);
    else kvStore.set(authKey, previousAuth);
    resetCodexAuthCacheForTest();
  }
});

Deno.test("openai: Terra Chat Completions accepts but omits the unsupported Codex completion cap", async () => {
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
            model: TERRA_TEST_MODEL,
            messages: [{ role: "user", content: "ping" }],
            temperature: 0,
            max_completion_tokens: 2048,
          }),
        }),
      ),
  );

  assert.equal(response.status, 200);
  const warnings = parseWarnings(response.headers.get("x-uos-warning"));
  assert.ok(warnings.includes("temperature_ignored"));
  assert.ok(warnings.includes("max_output_tokens_ignored"));
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal(recorded.model, TERRA_TEST_MODEL);
  assert.equal("max_output_tokens" in recorded, false);
  assert.equal("max_completion_tokens" in recorded, false);
  assert.equal("temperature" in recorded, false);
});

Deno.test("openai: prompt-cache sessions are stable within and isolated across authenticated principals", async () => {
  const identities: Array<Record<string, string | null>> = [];
  const responseStatuses = await withFetchMock(
    (_url, _bodyText, init) => {
      const headers = new Headers(init?.headers);
      identities.push({
        conversation: headers.get("conversation_id"),
        session: headers.get("session-id"),
        thread: headers.get("thread-id"),
        clientRequest: headers.get("x-client-request-id"),
      });
      return sseResponse(baseSseChunks());
    },
    async () => {
      const invoke = async (principal: string): Promise<number> => {
        const response = await handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              input: "stable prefix",
              prompt_cache_key: "shared-client-key",
            }),
          }),
          {
            keyId: null,
            kernelRepo: null,
            kernelOrg: null,
            idempotencyPrincipal: principal,
          },
        );
        return response.status;
      };
      return [await invoke("api-key:one"), await invoke("api-key:one"), await invoke("api-key:two")];
    },
  );

  assert.deepEqual(responseStatuses, [200, 200, 200]);
  assert.equal(identities.length, 3);
  assert.deepEqual(identities[1], identities[0]);
  assert.notEqual(identities[2]?.conversation, identities[0]?.conversation);
  for (const identity of identities) {
    assert.match(identity.conversation ?? "", /^[0-9a-f]{8}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    assert.equal(identity.session, identity.conversation);
    assert.equal(identity.thread, identity.conversation);
    assert.equal(identity.clientRequest, identity.conversation);
  }
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

Deno.test("openai: models omits provider models without OpenAI inference endpoints", async () => {
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-model-filter-test-key");
  Deno.env.delete("SURPLUS_API_KEY");
  try {
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [
            {
              id: "openlux-responses-model",
              owned_by: "openlux",
              supported_endpoint_types: ["openai-response"],
            },
            {
              id: "openlux-chat-model",
              owned_by: "openlux",
              supported_endpoint_types: ["openai"],
            },
            {
              id: "gpt-image-2",
              model_type: "图像",
              owned_by: "openlux",
              supported_endpoint_types: ["image-generation"],
            },
          ],
        })),
    });

    const response = await handleModels();
    assert.equal(response.status, 200);
    const payload = await response.json() as {
      object?: unknown;
      data?: Array<Record<string, unknown> & { id?: string }>;
    };
    assert.deepEqual(Object.keys(payload).sort(), ["data", "object"]);
    assert.equal(payload.object, "list");
    assert.ok(Array.isArray(payload.data));
    const modelIds = new Set(payload.data.map((model) => model.id));
    assert.equal(modelIds.has("openlux-responses-model"), true);
    assert.equal(modelIds.has("openlux-chat-model"), true);
    assert.equal(modelIds.has("gpt-image-2"), false);
    for (const model of payload.data) {
      assert.deepEqual(Object.keys(model).sort(), ["created", "id", "object", "owned_by"]);
    }
  } finally {
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: public catalog hides OpenLux-only models", async () => {
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  const originalMeteredKey = Deno.env.get("METERED_API_KEY");
  const runtimeConfigKey = keyToString(["uos_ai", "runtime_config", "v2"]);
  const previousRuntimeConfig = kvStore.get(runtimeConfigKey);
  const snapshot = kvStore.get(keyToString(TEST_CODEX_MODELS_KEY));
  kvStore.set(runtimeConfigKey, {
    version: 2,
    default_model: DEFAULT_TEST_MODEL,
    default_reasoning_effort: "low",
    codex_models: snapshot,
    updated_at_ms: Date.now(),
  });
  resetRuntimeConfigCacheForTest();
  Deno.env.set("METERED_API_KEY", "metered-public-catalog-test-key");
  await fetchMeteredModels({
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [
          {
            id: TERRA_TEST_MODEL,
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
          },
          {
            id: "openlux-surplus-shared-model",
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
          },
          {
            id: "openlux-only-model",
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
          },
        ],
      })),
  });
  await fetchSurplusModels({
    apiKey: "surplus-public-catalog-test-key",
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [
          { id: "openlux-surplus-shared-model", provider: "surplus" },
          { id: "surplus-only-model", provider: "surplus" },
        ],
      })),
  });

  try {
    const response = await handlePublicModelCatalog();
    assert.equal(response.status, 200);
    const payload = await response.json() as {
      data?: Array<{ id?: string; providers?: Array<{ id?: string }> }>;
      sources?: { openlux?: { count?: number } };
    };
    const byId = new Map((payload.data ?? []).map((model) => [model.id, model]));
    assert.deepEqual(byId.get(TERRA_TEST_MODEL)?.providers?.map((provider) => provider.id), ["codex", "openlux"]);
    assert.deepEqual(
      byId.get("openlux-surplus-shared-model")?.providers?.map((provider) => provider.id),
      ["openlux", "surplus"],
    );
    assert.equal(byId.has("openlux-only-model"), false);
    assert.deepEqual(byId.get("surplus-only-model")?.providers?.map((provider) => provider.id), ["surplus"]);
    assert.equal(payload.sources?.openlux?.count, 2);
  } finally {
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (previousRuntimeConfig === undefined) kvStore.delete(runtimeConfigKey);
    else kvStore.set(runtimeConfigKey, previousRuntimeConfig);
    resetRuntimeConfigCacheForTest();
    if (originalMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredKey);
  }
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
          probe_profile: "responses_implicit_input_text_keyed_cycle_isolated_v5",
          account_slots: "shared",
          token_refresh: "preserved",
          conversation_id: "independent",
          reproducible_cycles: 3,
          source: "live_probe",
          verified_at_ms: 2_001,
        },
      },
      {
        id: "metered",
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
  const envKey = "CEREBRAS_API_KEY";
  const originalApiKey = Deno.env.get(envKey);
  kvStore.delete(snapshotKey);
  Deno.env.delete(envKey);

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
    if (originalApiKey === undefined) Deno.env.delete(envKey);
    else Deno.env.set(envKey, originalApiKey);
  }
});

Deno.test("openai: configured Cerebras GPT-OSS is discoverable without altering the Codex catalog", async () => {
  const envKey = "CEREBRAS_API_KEY";
  const originalApiKey = Deno.env.get(envKey);
  Deno.env.set(envKey, "cerebras-test-key");
  try {
    const models = await handleModels();
    assert.equal(models.status, 200);
    const modelList = await models.json() as { data?: Array<Record<string, unknown>> };
    const model = modelList.data?.find((entry) => entry.id === "gpt-oss-120b");
    assert.deepEqual(model, {
      id: "gpt-oss-120b",
      object: "model",
      created: 0,
      owned_by: "cerebras",
    });

    const capabilities = await handleModelCapabilities();
    assert.equal(capabilities.status, 200);
    const capabilityList = await capabilities.json() as { data?: Array<Record<string, unknown>> };
    assert.deepEqual(
      capabilityList.data?.find((entry) => entry.id === "gpt-oss-120b"),
      {
        id: "gpt-oss-120b",
        object: "uos.model_capabilities",
        owned_by: "cerebras",
        display_name: "GPT-OSS 120B",
        upstream_provider: "cerebras",
        supported_endpoints: ["/v1/chat/completions"],
        supported_reasoning_levels: ["low", "medium", "high"],
        default_reasoning_effort: "medium",
        reasoning_effort_wire_map: {},
        context_window_tokens: null,
        max_context_window_tokens: null,
        auto_compact_token_limit_tokens: null,
      },
    );
  } finally {
    if (originalApiKey === undefined) Deno.env.delete(envKey);
    else Deno.env.set(envKey, originalApiKey);
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
      if (testCase.route === "responses" && testCase.status >= 500) {
        const telemetry = getResponseTelemetry(response);
        assert.ok(telemetry);
        assert.equal(telemetry.failureKind, "upstream_http_5xx");
        assert.equal(telemetry.streamTerminalType, "error");
        assert.equal(telemetry.responseCreatedObserved, false);
        assert.equal(telemetry.fallbackReason, null);
      }
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

Deno.test("openai: request abort after Codex headers releases its half-open probe neutrally", async () => {
  const accountId = "acct-cancelled-probe-fixture";
  const authPoolKey = keyToString(["ubq_ai", "codex_auth"]);
  const routingKey = keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY);
  const healthKey = keyToString(["uos_ai", "provider_health", "v1", "codex", accountId, "current"]);
  const upstreamErrorHealthKey = keyToString([
    "uos_ai",
    "provider_health",
    "v1",
    "codex",
    accountId,
    "upstream_error",
  ]);
  const isFixtureHealthKey = (encoded: string): boolean => {
    const key = JSON.parse(encoded) as unknown[];
    return key[0] === "uos_ai" && key[1] === "provider_health" && key[2] === "v1" && key[3] === "codex" &&
      key[4] === accountId;
  };
  const clearFixtureHealth = (): void => {
    for (const encoded of [...kvStore.keys()]) {
      if (isFixtureHealthKey(encoded)) kvStore.delete(encoded);
    }
  };
  const previousAuthPool = kvStore.get(authPoolKey);
  const previousRouting = kvStore.get(routingKey);
  const awaitingSemantic = new Deferred<void>();
  let releaseBlockedPull = (): void => {};
  let upstreamCancellations = 0;
  let codexCalls = 0;
  const waitForLeaseRelease = async (label: string): Promise<void> => {
    const deadline = performance.now() + 1_000;
    while (true) {
      const routing = kvStore.get(routingKey) as { slots?: Array<{ probe_lease?: unknown }> } | undefined;
      if (routing?.slots?.[0]?.probe_lease === null) return;
      if (performance.now() >= deadline) assert.fail(`${label} did not release its half-open lease`);
      await new Promise<void>((resolve) => setTimeout(resolve, 0));
    }
  };

  try {
    await withFetchMock(
      () => {
        codexCalls += 1;
        if (codexCalls !== 1) {
          return sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.incomplete",
                response: { id: "resp_cancelled_probe_retry", status: "incomplete", output: [] },
              })
            }\n\n`,
          ]);
        }
        let emittedCreated = false;
        return new Response(
          new ReadableStream<Uint8Array>({
            pull(controller) {
              if (!emittedCreated) {
                emittedCreated = true;
                controller.enqueue(
                  TEXT_ENCODER.encode(
                    `data: ${
                      JSON.stringify({ type: "response.created", response: { id: "resp_cancelled_probe" } })
                    }\n\n`,
                  ),
                );
                return;
              }
              awaitingSemantic.resolve();
              return new Promise<void>((resolve) => {
                releaseBlockedPull = resolve;
              });
            },
            cancel() {
              upstreamCancellations += 1;
              releaseBlockedPull();
            },
          }),
          {
            status: 200,
            headers: { "Content-Type": "text/event-stream", "X-Request-Id": "cancelled-probe-request" },
          },
        );
      },
      async () => {
        const existingPool = kvStore.get(authPoolKey) as {
          accounts: Array<{
            access_token: string;
            refresh_token: string;
            account_id: string;
          }>;
          updated_at_ms: number;
        };
        const pool = {
          ...existingPool,
          accounts: existingPool.accounts.map((account, index) =>
            index === 0 ? { ...account, account_id: accountId } : account
          ),
          updated_at_ms: Date.now(),
        };
        kvStore.set(authPoolKey, pool);
        resetCodexAuthCacheForTest();
        resetProviderHealthThrottleForTest();
        clearFixtureHealth();
        const account = pool.accounts[0]!;
        const credentialVersion = await sha256Hex(
          `${account.account_id}\u0000${account.access_token}\u0000${account.refresh_token}`,
        );
        kvStore.set(routingKey, {
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
        resetCodexAccountRoutingForTest();

        const abortController = new AbortController();
        const cancelledResponse = handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "cancel half-open probe", stream: true }),
            signal: abortController.signal,
          }),
        );
        await awaitingSemantic.promise;
        const claimed = kvStore.get(routingKey) as { slots?: Array<{ probe_lease?: unknown }> } | undefined;
        assert.ok(claimed?.slots?.[0]?.probe_lease, "the in-flight 2xx response must own the half-open lease");

        abortController.abort(new DOMException("client cancelled", "AbortError"));
        const cancelled = await cancelledResponse;
        assert.equal(cancelled.status, 499);
        await cancelled.text();
        assert.equal(upstreamCancellations, 1);

        await recordCodexProviderHealth(accountId, "reachable", 299, Date.now, "cancel-barrier");
        await waitForLeaseRelease("the cancelled response");
        assert.equal(kvStore.get(upstreamErrorHealthKey), undefined);
        const cancellationHealth = kvStore.get(healthKey) as
          | { event?: unknown; provider_request_id?: unknown }
          | undefined;
        assert.equal(cancellationHealth?.event, "reachable");
        assert.equal(cancellationHealth?.provider_request_id, "cancel-barrier");

        const retry = await handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "retry after cancellation" }),
          }),
        );
        assert.equal(retry.status, 200);
        await retry.text();
        assert.equal(codexCalls, 2);
        await waitForLeaseRelease("the neutral retry");
        await recordCodexProviderHealth(accountId, "reachable", 299, Date.now, "retry-barrier");
        assert.equal(
          kvStore.get(upstreamErrorHealthKey),
          undefined,
          "neutral cancellation and incompletion must not write upstream-error health",
        );
      },
    );
  } finally {
    if (previousAuthPool === undefined) kvStore.delete(authPoolKey);
    else kvStore.set(authPoolKey, previousAuthPool);
    if (previousRouting === undefined) kvStore.delete(routingKey);
    else kvStore.set(routingKey, previousRouting);
    clearFixtureHealth();
    resetProviderHealthThrottleForTest();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
  }
});

Deno.test("openai: buffered inference deadline after Codex headers records an upstream failure", async () => {
  const accountId = "acct-buffered-deadline-fixture";
  const authPoolKey = keyToString(["ubq_ai", "codex_auth"]);
  const routingKey = keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY);
  const healthKey = keyToString(["uos_ai", "provider_health", "v1", "codex", accountId, "current"]);
  const previousAuthPool = kvStore.get(authPoolKey);
  const previousRouting = kvStore.get(routingKey);
  const originalTimeout = AbortSignal.timeout;
  const inferenceDeadline = new AbortController();
  const awaitingSemantic = new Deferred<void>();
  let releaseBlockedPull = (): void => {};
  let upstreamCancellations = 0;
  const isFixtureHealthKey = (encoded: string): boolean => {
    const key = JSON.parse(encoded) as unknown[];
    return key[0] === "uos_ai" && key[1] === "provider_health" && key[2] === "v1" && key[3] === "codex" &&
      key[4] === accountId;
  };
  const clearFixtureHealth = (): void => {
    for (const encoded of [...kvStore.keys()]) {
      if (isFixtureHealthKey(encoded)) kvStore.delete(encoded);
    }
  };

  try {
    (AbortSignal as unknown as { timeout: (milliseconds: number) => AbortSignal }).timeout = () =>
      inferenceDeadline.signal;
    await withFetchMock(
      () => {
        let emittedCreated = false;
        return new Response(
          new ReadableStream<Uint8Array>({
            pull(controller) {
              if (!emittedCreated) {
                emittedCreated = true;
                controller.enqueue(
                  TEXT_ENCODER.encode(
                    `data: ${
                      JSON.stringify({ type: "response.created", response: { id: "resp_buffered_deadline" } })
                    }\n\n`,
                  ),
                );
                return;
              }
              awaitingSemantic.resolve();
              return new Promise<void>((resolve) => {
                releaseBlockedPull = resolve;
              });
            },
            cancel() {
              upstreamCancellations += 1;
              releaseBlockedPull();
            },
          }),
          {
            status: 200,
            headers: { "Content-Type": "text/event-stream", "X-Request-Id": "buffered-deadline-request" },
          },
        );
      },
      async () => {
        const existingPool = kvStore.get(authPoolKey) as CodexAuthPoolState;
        kvStore.set(authPoolKey, {
          ...existingPool,
          accounts: existingPool.accounts.map((account, index) =>
            index === 0 ? { ...account, account_id: accountId } : account
          ),
          updated_at_ms: Date.now(),
        });
        kvStore.delete(routingKey);
        clearFixtureHealth();
        resetCodexAuthCacheForTest();
        resetCodexAccountRoutingForTest();
        resetProviderHealthThrottleForTest();

        const downstreamRequest = new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "buffered deadline" }),
        });
        const pending = handleResponses(downstreamRequest);
        await awaitingSemantic.promise;
        assert.equal(downstreamRequest.signal.aborted, false);

        inferenceDeadline.abort(new DOMException("buffered inference timed out", "TimeoutError"));
        const response = await pending;
        assert.equal(response.status, 504);
        assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
        await response.text();
        assert.equal(upstreamCancellations, 1);

        const healthDeadline = performance.now() + 1_000;
        while (true) {
          const health = kvStore.get(healthKey) as
            | { event?: unknown; status?: unknown; provider_request_id?: unknown }
            | undefined;
          if (health?.event === "upstream_error") {
            assert.equal(health.status, 200);
            assert.equal(health.provider_request_id, "buffered-deadline-request");
            break;
          }
          if (performance.now() >= healthDeadline) {
            assert.fail("the buffered deadline was not recorded as an upstream failure");
          }
          await new Promise<void>((resolve) => setTimeout(resolve, 0));
        }
      },
    );
  } finally {
    (AbortSignal as unknown as { timeout: (milliseconds: number) => AbortSignal }).timeout = originalTimeout;
    if (previousAuthPool === undefined) kvStore.delete(authPoolKey);
    else kvStore.set(authPoolKey, previousAuthPool);
    if (previousRouting === undefined) kvStore.delete(routingKey);
    else kvStore.set(routingKey, previousRouting);
    clearFixtureHealth();
    resetProviderHealthThrottleForTest();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
  }
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

Deno.test("openai: reasoning progress releases streaming headers before semantic output", async (t) => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const debugKey = keyToString(DEBUG_ROUTING_KEY);
  const previousDebugRouting = kvStore.get(debugKey);
  const deadlineMs = 300;
  const routeCases = [
    { route: "responses", keyId: "reasoning-progress-responses" },
    { route: "chat", keyId: "reasoning-progress-chat" },
  ] as const;

  const progressingReasoningResponse = (
    responseId: string,
    observation: { reasoningEmitted: boolean; semanticEmitted: boolean },
  ): { response: Response; releaseSemantic: () => void } => {
    let stopped = false;
    const semanticGate = Promise.withResolvers<void>();
    const stream = new ReadableStream<Uint8Array>({
      async start(controller) {
        const enqueue = (value: Record<string, unknown>): void => {
          if (stopped) return;
          const type = String(value.type ?? "");
          if (type.startsWith("response.reasoning_")) observation.reasoningEmitted = true;
          if (type === "response.output_text.delta") observation.semanticEmitted = true;
          controller.enqueue(TEXT_ENCODER.encode(`data: ${JSON.stringify(value)}\n\n`));
        };

        enqueue({
          type: "response.created",
          response: { id: responseId, object: "response", status: "in_progress", output: [] },
        });
        enqueue({
          type: "response.reasoning_summary_text.delta",
          response_id: responseId,
          item_id: `reasoning_${responseId}`,
          output_index: 0,
          summary_index: 0,
          delta: "hidden summary progress",
        });
        enqueue({
          type: "response.reasoning_text.delta",
          response_id: responseId,
          item_id: `reasoning_${responseId}`,
          output_index: 0,
          content_index: 0,
          delta: "hidden reasoning progress",
        });

        await semanticGate.promise;
        if (stopped) return;
        enqueue({
          type: "response.output_text.delta",
          response_id: responseId,
          item_id: `message_${responseId}`,
          output_index: 0,
          content_index: 0,
          delta: "progress complete",
        });
        enqueue({
          type: "response.completed",
          response: {
            id: responseId,
            object: "response",
            status: "completed",
            model: DEFAULT_TEST_MODEL,
            output: [{
              id: `message_${responseId}`,
              type: "message",
              status: "completed",
              role: "assistant",
              content: [{ type: "output_text", text: "progress complete", annotations: [] }],
            }],
            usage: { input_tokens: 3, output_tokens: 2, total_tokens: 5 },
          },
        });
        stopped = true;
        controller.close();
      },
      cancel() {
        stopped = true;
        semanticGate.resolve();
      },
    });
    return {
      response: new Response(stream, { status: 200, headers: { "Content-Type": "text/event-stream" } }),
      releaseSemantic: semanticGate.resolve,
    };
  };

  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.delete("SURPLUS_API_KEY");
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai", "openai-response"] }],
        })),
    });
    setStreamFirstEventDeadlineMsForTest(deadlineMs);

    for (const routeCase of routeCases) {
      await t.step(`${routeCase.route} stays alive through hidden reasoning`, async () => {
        const { keyId } = routeCase;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        let codexCalls = 0;
        const observation = { reasoningEmitted: false, semanticEmitted: false };
        let releaseSemantic: (() => void) | null = null;
        const response = await withFetchMock(
          (url) => {
            if (url !== "https://chatgpt.com/backend-api/codex/responses") {
              throw new Error(`Reasoning progress must not change providers: ${url}`);
            }
            codexCalls += 1;
            const upstream = progressingReasoningResponse(`resp_${routeCase.route}_reasoning_progress`, observation);
            releaseSemantic = upstream.releaseSemantic;
            return upstream.response;
          },
          () => {
            const usageContext = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: true,
              requestId,
              startedAtMs: Date.now(),
            };
            return routeCase.route === "responses"
              ? handleResponses(responsesRequest({ stream: true }), usageContext)
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    stream: true,
                    messages: [{ role: "user", content: "work through this carefully" }],
                  }),
                }),
                usageContext,
              );
          },
        );

        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
        assert.equal(observation.reasoningEmitted, true);
        assert.equal(observation.semanticEmitted, false);
        await new Promise((resolve) => setTimeout(resolve, deadlineMs + 50));
        assert.equal(observation.semanticEmitted, false);
        assert.notEqual(releaseSemantic, null);
        releaseSemantic!();
        const serialized = await response.text();
        assert.equal(observation.semanticEmitted, true);
        assert.match(serialized, /progress complete/);
        if (routeCase.route === "responses") {
          const values = parseResponsesSseValues(serialized);
          assert.equal(values.filter((event) => event.type === "response.completed").length, 1);
          assert.ok(values.some((event) => event.type === "response.reasoning_summary_text.delta"));
          assert.ok(values.some((event) => event.type === "response.reasoning_text.delta"));
        } else {
          assert.equal(serialized.match(/data: \[DONE\]/g)?.length, 1);
        }
        assert.equal(codexCalls, 1);
        assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      });
    }

    kvStore.set(debugKey, {
      scenario: "normal",
      expires_at_ms: null,
      updated_at_ms: Date.now(),
    });
    resetDebugRoutingCacheForTest();
    for (const routeCase of routeCases) {
      await t.step(`${routeCase.route} keeps Metered alive through hidden reasoning`, async () => {
        const keyId = `paid-${routeCase.keyId}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        const observation = { reasoningEmitted: false, semanticEmitted: false };
        let releaseSemantic: (() => void) | null = null;
        const response = await withFetchMock(
          (url) => {
            if (url === "https://chatgpt.com/backend-api/codex/responses") {
              return authoritativeCodexQuotaResponse();
            }
            if (url !== "https://api.openlux.ai/v1/responses") {
              throw new Error(`Unexpected provider during Metered reasoning progress: ${url}`);
            }
            const upstream = progressingReasoningResponse(`resp_${routeCase.route}_metered_reasoning`, observation);
            releaseSemantic = upstream.releaseSemantic;
            return upstream.response;
          },
          () => {
            const usageContext = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: true,
              requestId,
              startedAtMs: Date.now(),
            };
            return routeCase.route === "responses"
              ? handleResponses(responsesRequest({ stream: true }), usageContext)
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    stream: true,
                    messages: [{ role: "user", content: "work through this carefully" }],
                  }),
                }),
                usageContext,
              );
          },
        );

        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-uos-upstream"), "metered");
        assert.equal(observation.reasoningEmitted, true);
        assert.equal(observation.semanticEmitted, false);
        await new Promise((resolve) => setTimeout(resolve, deadlineMs + 50));
        assert.equal(observation.semanticEmitted, false);
        assert.notEqual(releaseSemantic, null);
        releaseSemantic!();
        const serialized = await response.text();
        assert.equal(observation.semanticEmitted, true);
        assert.match(serialized, /progress complete/);
        const stored = await waitForPaidFallbackTerminal(keyId, requestId, "completed");
        assert.equal(stored.provider, "metered");
        kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
        kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
      });
    }
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
    if (previousDebugRouting === undefined) kvStore.delete(debugKey);
    else kvStore.set(debugKey, previousDebugRouting);
    resetDebugRoutingCacheForTest();
    for (const { keyId } of routeCases) {
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", `paid-${keyId}`]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-paid-${keyId}`]));
    }
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: paid-provider reasoning progress releases only streaming requests", async (t) => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const deadlineMs = 150;

  try {
    setStreamFirstEventDeadlineMsForTest(deadlineMs);
    for (const provider of ["surplus", "metered"] as const) {
      resetMeteredModelsCacheForTest();
      resetSurplusModelsCacheForTest();
      if (provider === "surplus") {
        Deno.env.delete("METERED_API_KEY");
        Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
        await fetchSurplusModels({
          apiKey: "surplus-test-key",
          force: true,
          fetcher: () =>
            Promise.resolve(Response.json({
              data: [{
                id: DEFAULT_TEST_MODEL,
                pricing: { prompt: 0.000001, completion: 0.000003 },
              }],
            })),
        });
      } else {
        Deno.env.set("METERED_API_KEY", "metered-test-key");
        Deno.env.delete("SURPLUS_API_KEY");
        await fetchMeteredModels({
          force: true,
          fetcher: () =>
            Promise.resolve(Response.json({
              data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai", "openai-response"] }],
            })),
        });
      }

      for (const route of ["responses", "chat"] as const) {
        for (const stream of [true, false]) {
          const delivery = stream ? "streaming" : "buffered";
          await t.step(`${provider} ${route} ${delivery}`, async () => {
            const keyId = `reasoning-progress-${provider}-${route}-${delivery}`;
            const requestId = `request-${keyId}`;
            seedPaidFallbackKey(keyId);
            const reasoningObserved = Promise.withResolvers<void>();
            const semanticGate = Promise.withResolvers<void>();
            let stopped = false;
            let semanticEmitted = false;
            let upstreamCancellations = 0;
            const originalTimeout = AbortSignal.timeout;
            const bufferedDeadline = stream ? null : new AbortController();
            if (bufferedDeadline) {
              (AbortSignal as unknown as { timeout: (milliseconds: number) => AbortSignal }).timeout = () =>
                bufferedDeadline.signal;
            }

            const upstreamResponse = (): Response =>
              new Response(
                new ReadableStream<Uint8Array>({
                  start(controller) {
                    const enqueue = (value: Record<string, unknown>): void => {
                      if (!stopped) {
                        controller.enqueue(TEXT_ENCODER.encode(`data: ${JSON.stringify(value)}\n\n`));
                      }
                    };
                    const responseId = `resp_${provider}_${route}_${delivery}`;
                    enqueue({
                      type: "response.created",
                      response: { id: responseId, object: "response", status: "in_progress", output: [] },
                    });
                    enqueue({
                      type: "response.reasoning_summary_text.delta",
                      response_id: responseId,
                      item_id: `reasoning_${responseId}`,
                      output_index: 0,
                      summary_index: 0,
                      delta: "recognized hidden reasoning progress",
                    });
                    reasoningObserved.resolve();

                    void semanticGate.promise.then(() => {
                      if (stopped) return;
                      semanticEmitted = true;
                      enqueue({
                        type: "response.output_text.delta",
                        response_id: responseId,
                        item_id: `message_${responseId}`,
                        output_index: 0,
                        content_index: 0,
                        delta: "paid progress complete",
                      });
                      enqueue({
                        type: "response.completed",
                        response: {
                          id: responseId,
                          object: "response",
                          status: "completed",
                          model: DEFAULT_TEST_MODEL,
                          output: [{
                            id: `message_${responseId}`,
                            type: "message",
                            status: "completed",
                            role: "assistant",
                            content: [{ type: "output_text", text: "paid progress complete", annotations: [] }],
                          }],
                          usage: { input_tokens: 3, output_tokens: 2, total_tokens: 5 },
                        },
                      });
                      stopped = true;
                      controller.close();
                    });
                  },
                  cancel() {
                    upstreamCancellations += 1;
                    stopped = true;
                    semanticGate.resolve();
                  },
                }),
                {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Api-Request-Id": `provider-${requestId}`,
                    "X-Oneapi-Request-Id": `provider-${requestId}`,
                  },
                },
              );

            try {
              await withFetchMock(
                (url) => {
                  if (url === "https://chatgpt.com/backend-api/codex/responses") {
                    return authoritativeCodexQuotaResponse();
                  }
                  if (
                    url ===
                      (provider === "surplus"
                        ? "https://api.surplusintelligence.ai/v1/responses"
                        : "https://api.openlux.ai/v1/responses")
                  ) {
                    return upstreamResponse();
                  }
                  if (url.startsWith("https://api.openlux.ai/api/log/token?")) {
                    return Response.json({ success: true, data: { items: [] } });
                  }
                  throw new Error(`Unexpected ${provider} reasoning-progress request: ${url}`);
                },
                async () => {
                  const usageContext = {
                    keyId,
                    kernelRepo: null,
                    kernelOrg: null,
                    paidFallbackEnabled: true,
                    requestId,
                    startedAtMs: Date.now(),
                  };
                  const pending = route === "responses"
                    ? handleResponses(responsesRequest({ stream }), usageContext)
                    : handleChatCompletions(
                      new Request("https://ai.ubq.fi/v1/chat/completions", {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({
                          model: DEFAULT_TEST_MODEL,
                          stream,
                          messages: [{ role: "user", content: "reason before answering" }],
                        }),
                      }),
                      usageContext,
                    );

                  await reasoningObserved.promise;
                  assert.equal(semanticEmitted, false);
                  bufferedDeadline?.abort(new DOMException("buffered inference timed out", "TimeoutError"));
                  const response = await pending;
                  assert.equal(response.headers.get("x-uos-upstream"), provider);
                  assert.notEqual(getResponseTelemetry(response)?.semanticOutputObserved, true);

                  if (stream) {
                    assert.equal(response.status, 200);
                    await new Promise((resolve) => setTimeout(resolve, deadlineMs + 50));
                    assert.equal(semanticEmitted, false);
                    semanticGate.resolve();
                    const serialized = await response.text();
                    assert.match(serialized, /paid progress complete/);
                    assert.equal(upstreamCancellations, 0);
                    await waitForPaidFallbackTerminal(keyId, requestId, "completed");
                  } else {
                    assert.equal(response.status, 504);
                    const payload = await response.json() as { error?: { code?: unknown } };
                    assert.equal(payload.error?.code, "gateway_timeout");
                    assert.equal(semanticEmitted, false);
                    await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
                    // Resolve the fixture gate even when a provider wrapper has
                    // already detached from the timed-out response body.
                    semanticGate.resolve();
                  }
                },
              );
            } finally {
              (AbortSignal as unknown as { timeout: (milliseconds: number) => AbortSignal }).timeout = originalTimeout;
            }

            for (const encodedKey of [...kvStore.keys()]) {
              const key = JSON.parse(encodedKey) as unknown[];
              if (key.includes(keyId)) kvStore.delete(encodedKey);
            }
            resetProviderHealthThrottleForTest();
          });
        }
      }
    }
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    kvStore.delete(keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY));
    resetCodexAccountRoutingForTest();
    resetProviderHealthThrottleForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: cancelling a reasoning-released Codex stream stays cancelled and unpaid", async (t) => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const routeCases = [
    { route: "responses", keyId: "reasoning-cancel-responses" },
    { route: "chat", keyId: "reasoning-cancel-chat" },
  ] as const;

  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");

    for (const routeCase of routeCases) {
      await t.step(`${routeCase.route} cancels after hidden reasoning releases headers`, async () => {
        const { keyId } = routeCase;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        let codexCalls = 0;
        let surplusCalls = 0;
        let meteredCalls = 0;
        let upstreamCancellations = 0;
        let releaseBlockedPull = (): void => {};
        const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];

        const response = await withFetchMock(
          (url) => {
            if (url === "https://chatgpt.com/backend-api/codex/responses") {
              codexCalls += 1;
              const responseId = `resp_${routeCase.route}_reasoning_cancel`;
              return new Response(
                new ReadableStream<Uint8Array>({
                  start(controller) {
                    controller.enqueue(
                      TEXT_ENCODER.encode(
                        `data: ${
                          JSON.stringify({
                            type: "response.created",
                            response: { id: responseId, object: "response", status: "in_progress", output: [] },
                          })
                        }\n\n` +
                          `data: ${
                            JSON.stringify({
                              type: "response.reasoning_summary_text.delta",
                              response_id: responseId,
                              item_id: `reasoning_${responseId}`,
                              output_index: 0,
                              summary_index: 0,
                              delta: "hidden progress before cancellation",
                            })
                          }\n\n`,
                      ),
                    );
                  },
                  pull() {
                    return new Promise<void>((resolve) => {
                      releaseBlockedPull = resolve;
                    });
                  },
                  cancel() {
                    upstreamCancellations += 1;
                    releaseBlockedPull();
                  },
                }),
                { status: 200, headers: { "Content-Type": "text/event-stream" } },
              );
            }
            if (url === "https://api.surplusintelligence.ai/v1/responses") {
              surplusCalls += 1;
              throw new Error("reasoning-progress cancellation must not dispatch to Surplus");
            }
            if (url === "https://api.openlux.ai/v1/responses") {
              meteredCalls += 1;
              throw new Error("reasoning-progress cancellation must not dispatch to OpenLux");
            }
            throw new Error(`Unexpected upstream dispatch during reasoning cancellation: ${url}`);
          },
          async () => {
            const usageContext = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: true,
              requestId,
              startedAtMs: Date.now(),
              onTerminalUsage: (usage: { inputTokens: number | null } | null, completed: boolean) => {
                observedTerminalUsages.push({ completed, inputTokens: usage?.inputTokens ?? null });
              },
            };
            const routed = routeCase.route === "responses"
              ? await handleResponses(responsesRequest({ stream: true }), usageContext)
              : await handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    stream: true,
                    messages: [{ role: "user", content: "reason until I cancel" }],
                  }),
                }),
                usageContext,
              );

            assert.equal(routed.status, 200);
            assert.equal(routed.headers.get("x-uos-upstream"), "chatgpt_codex");
            assert.ok(routed.body);
            await routed.body.cancel("client cancelled after reasoning progress");

            const cancellationDeadline = performance.now() + 1_000;
            while (
              upstreamCancellations === 0 || getResponseTelemetry(routed)?.streamTerminalType !== "cancelled"
            ) {
              if (performance.now() >= cancellationDeadline) {
                assert.fail(`${routeCase.route} did not finish its cancellation lifecycle`);
              }
              await new Promise<void>((resolve) => setTimeout(resolve, 0));
            }
            await new Promise<void>((resolve) => setTimeout(resolve, 0));
            return routed;
          },
        );

        const telemetry = getResponseTelemetry(response);
        assert.equal(telemetry?.provider, "chatgpt_codex");
        assert.equal(telemetry?.fallbackReason, null);
        assert.equal(telemetry?.streamTerminalType, "cancelled");
        assert.equal(telemetry?.completed, false);
        assert.notEqual(telemetry?.semanticOutputObserved, true);
        assert.deepEqual(observedTerminalUsages, []);
        assert.equal(upstreamCancellations, 1);
        assert.equal(codexCalls, 1);
        assert.equal(surplusCalls, 0);
        assert.equal(meteredCalls, 0);
        assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);

        const keyRecord = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
          usage_reset_at_ms: number;
          paid_fallback_spent_microcredits: number;
          paid_fallback_reserved_microcredits: number;
          paid_fallback_reservation_request_id: string | null;
        };
        assert.equal(keyRecord.paid_fallback_spent_microcredits, 0);
        assert.equal(keyRecord.paid_fallback_reserved_microcredits, 0);
        assert.equal(keyRecord.paid_fallback_reservation_request_id, null);
        assert.equal(
          kvStore.get(
            keyToString(["uos_ai", "paid_fallback", "v3", "window", keyId, keyRecord.usage_reset_at_ms]),
          ),
          undefined,
        );
      });
    }
  } finally {
    for (const { keyId } of routeCases) {
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    }
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: streaming Responses clear their absolute deadline after semantic output", async () => {
  setStreamFirstEventDeadlineMsForTest(30);
  try {
    const response = await withFetchMock(
      () =>
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(TEXT_ENCODER.encode(
                `data: ${
                  JSON.stringify({
                    type: "response.created",
                    response: { id: "resp_stream_absolute", object: "response", status: "in_progress", output: [] },
                  })
                }\n\n`,
              ));
              controller.enqueue(TEXT_ENCODER.encode(
                `data: ${
                  JSON.stringify({
                    type: "response.output_text.delta",
                    response_id: "resp_stream_absolute",
                    item_id: "msg_stream_absolute",
                    output_index: 0,
                    content_index: 0,
                    delta: "still streaming",
                  })
                }\n\n`,
              ));
              setTimeout(() =>
                controller.enqueue(TEXT_ENCODER.encode(
                  `data: ${
                    JSON.stringify({
                      type: "response.completed",
                      response: {
                        id: "resp_stream_absolute",
                        object: "response",
                        status: "completed",
                        output: [],
                        usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                      },
                    })
                  }\n\n`,
                )), 60);
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        ),
      () => handleResponses(responsesRequest()),
    );
    assert.equal(response.status, 200);
    const values = parseResponsesSseValues(await response.text());
    assert.equal(values.filter((event) => event.type === "response.completed").length, 1);
    assert.equal(values.filter((event) => event.type === "response.failed").length, 0);
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

Deno.test("openai: transient Codex stalls never advance to paid fallback", async (t) => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const keyIds = [
    "fallback-codex-no-headers",
    "fallback-codex-no-semantic-event",
    "fallback-codex-post-semantic-eof",
  ];
  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.delete("SURPLUS_API_KEY");
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response"] }],
        })),
    });
    setStreamFirstEventDeadlineMsForTest(160);

    await t.step("no response headers returns Codex timeout and the next request retries Codex", async () => {
      const keyId = keyIds[0]!;
      const firstRequestId = `request-${keyId}`;
      seedPaidFallbackKey(keyId);
      let codexCalls = 0;
      let meteredCalls = 0;
      await withFetchMock(
        (url, _bodyText, init) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
            return sseResponse(baseSseChunks());
          }
          codexCalls += 1;
          if (codexCalls === 1) {
            return new Promise<Response>((_resolve, reject) => {
              const signal = init?.signal;
              if (!signal) return reject(new Error("Codex timeout fixture did not receive a signal"));
              const rejectWithReason = () => reject(signal.reason);
              if (signal.aborted) rejectWithReason();
              else signal.addEventListener("abort", rejectWithReason, { once: true });
            });
          }
          return sseResponse(baseSseChunks());
        },
        async () => {
          const first = await handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId: firstRequestId,
            startedAtMs: Date.now(),
          });
          assert.equal(first.status, 504);
          assert.equal(first.headers.get("x-uos-upstream"), "chatgpt_codex");
          assert.equal(getResponseTelemetry(first)?.fallbackReason, null);
          assert.equal(getStoredPaidFallbackRequest(keyId, firstRequestId), null);

          const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
          const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
          assert.equal(selection.kind, "eligible");

          const second = await handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId: `${firstRequestId}-next`,
            startedAtMs: Date.now(),
          });
          assert.equal(second.status, 200);
          assert.equal(second.headers.get("x-uos-upstream"), "chatgpt_codex");
          await second.text();
        },
      );
      assert.equal(codexCalls, 2);
      assert.equal(meteredCalls, 0);
    });

    await t.step("buffered setup events never leak when a pre-semantic Codex stream stalls", async () => {
      const keyId = keyIds[1]!;
      const requestId = `request-${keyId}`;
      seedPaidFallbackKey(keyId);
      let codexCalls = 0;
      let meteredCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
            return sseResponse(baseSseChunks());
          }
          codexCalls += 1;
          if (codexCalls > 1) return sseResponse(baseSseChunks());
          return new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                controller.enqueue(TEXT_ENCODER.encode(
                  `data: ${
                    JSON.stringify({
                      type: "response.created",
                      response: { id: "resp_codex_stalled", created_at: 0 },
                    })
                  }\n\n`,
                ));
              },
            }),
            {
              status: 200,
              headers: { "Content-Type": "text/event-stream" },
            },
          );
        },
        () =>
          handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId,
            startedAtMs: Date.now(),
          }),
      );
      assert.equal(response.status, 504);
      assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
      assert.equal(getResponseTelemetry(response)?.fallbackReason, null);
      const body = await response.text();
      assert.equal(body.includes("resp_codex_stalled"), false);
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);

      const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
      const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
      assert.equal(selection.kind, "eligible");

      const next = await withFetchMock(
        (url) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
            return sseResponse(baseSseChunks());
          }
          codexCalls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId: `${requestId}-next`,
            startedAtMs: Date.now(),
          }),
      );
      assert.equal(next.status, 200);
      assert.equal(next.headers.get("x-uos-upstream"), "chatgpt_codex");
      await next.text();
      assert.equal(codexCalls, 2);
      assert.equal(meteredCalls, 0);
    });

    await t.step("a stream failure after semantic output never switches providers", async () => {
      const keyId = keyIds[2]!;
      seedPaidFallbackKey(keyId);
      let meteredCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
            throw new Error("a committed Codex stream must not switch providers");
          }
          return sseResponse([
            `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_codex_committed" } })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.output_text.delta",
                response_id: "resp_codex_committed",
                item_id: "msg_codex_committed",
                output_index: 0,
                content_index: 0,
                delta: "partial",
              })
            }\n\n`,
          ]);
        },
        () =>
          handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId: `request-${keyId}`,
            startedAtMs: Date.now(),
          }),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
      const events = parseResponsesSseValues(await response.text());
      assert.equal(events.filter((event) => event.type === "response.created").length, 1);
      assert.equal(events.filter((event) => event.type === "response.failed").length, 1);
      assert.equal(meteredCalls, 0);
    });
  } finally {
    setStreamFirstEventDeadlineMsForTest(null);
    for (const keyId of keyIds) {
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    }
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
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

Deno.test("openai: a generic post-reset 429 does not authorize paid fallback", async () => {
  const previousMeteredKey = Deno.env.get("METERED_API_KEY");
  const keyId = "fallback-post-reset-generic-429";
  const requestId = "request-post-reset-generic-429";
  const providerCalls: string[] = [];
  let codexCalls = 0;
  let meteredCalls = 0;
  const provider: CodexUsageResetProvider = {
    contract: {
      idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
      lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
      verification: { independentlyVerifiable: true },
      receiptIdsSafeToPersistAndLog: false,
      supportedResetTypes: ["codex_rate_limits"],
    },
    readInventory: () => {
      providerCalls.push("inventory");
      return Promise.resolve({
        availableCount: 1,
        observedAtMs: Date.now(),
        credits: [{ id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null }],
      });
    },
    redeem: () => {
      providerCalls.push("redeem");
      return Promise.resolve({ kind: "completed", providerReceiptId: "post-reset-generic-receipt" } as const);
    },
    lookup: () => {
      providerCalls.push("lookup");
      return Promise.resolve({ kind: "completed", providerReceiptId: "post-reset-generic-receipt" } as const);
    },
    verifyApplied: () => {
      providerCalls.push("verify");
      return Promise.resolve(true);
    },
  };

  Deno.env.set("METERED_API_KEY", "metered-test-key");
  resetMeteredModelsCacheForTest();
  await fetchMeteredModels({
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response"] }],
      })),
  });
  seedPaidFallbackKey(keyId);

  try {
    const response = await withFetchMock(
      (url) => {
        if (url === "https://chatgpt.com/backend-api/codex/responses") {
          codexCalls += 1;
          if (codexCalls === 1) return authoritativeCodexQuotaResponse();
          return new Response(JSON.stringify({ error: { message: "Still limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        }
        if (url === "https://api.openlux.ai/v1/responses") {
          meteredCalls += 1;
          return sseResponse(baseSseChunks());
        }
        throw new Error(`Unexpected upstream dispatch in post-reset fallback test: ${url}`);
      },
      async () => {
        clearBankedResetRecords();
        setCodexBankedResetOptionsForTest({
          config: liveBankedResetFixtureConfig(),
          provider,
          kv: kvStub,
          now: () => Date.now(),
          newOwnerToken: () => "post-reset-generic-owner",
        });
        try {
          return await handleResponses(responsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId,
            startedAtMs: Date.now(),
          });
        } finally {
          setCodexBankedResetOptionsForTest(null);
          clearBankedResetRecords();
        }
      },
    );

    assert.equal(response.status, 429);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.equal(codexCalls, 2);
    assert.equal(meteredCalls, 0);
    assert.deepEqual(providerCalls, ["inventory", "redeem", "verify"]);
  } finally {
    setCodexBankedResetOptionsForTest(null);
    clearBankedResetRecords();
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    resetMeteredModelsCacheForTest();
    if (previousMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", previousMeteredKey);
  }
});

Deno.test("openai: an all-blocked Codex response continues through paid Metered fallback", async () => {
  const authKey = keyToString(["ubq_ai", "codex_auth"]);
  const previousAuth = kvStore.get(authKey);
  const previousMeteredKey = Deno.env.get("METERED_API_KEY");
  const keyId = "fallback-gateway-codex-quota";
  const requestId = "request-gateway-codex-quota";
  const now = Date.now();
  const expectedRetryAtMs = Math.floor((now + 60_000) / 1_000) * 1_000;
  const authPool: CodexAuthPoolState = {
    accounts: [{
      access_token: "access-one",
      refresh_token: "refresh-one",
      account_id: "account-one",
      updated_at_ms: now,
    }, {
      access_token: "access-two",
      refresh_token: "refresh-two",
      account_id: "account-two",
      updated_at_ms: now,
    }],
    updated_at_ms: now,
  };
  let meteredCalls = 0;
  Deno.env.set("METERED_API_KEY", "metered-test-key");
  resetMeteredModelsCacheForTest();
  await fetchMeteredModels({
    force: true,
    fetcher: () =>
      Promise.resolve(Response.json({
        data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response"] }],
      })),
  });
  seedPaidFallbackKey(keyId);

  try {
    await withFetchMock(
      (url) => {
        if (url === "https://api.openlux.ai/v1/responses") {
          meteredCalls += 1;
          return sseResponse(baseSseChunks());
        }
        throw new Error(`Unexpected upstream dispatch in all-blocked routing test: ${url}`);
      },
      async () => {
        kvStore.set(authKey, authPool);
        resetCodexAuthCacheForTest();
        const selection = await selectCodexRoutingAccounts(authPool, authPool.accounts, now);
        assert.equal(selection.kind, "eligible");
        if (selection.kind !== "eligible") return;
        for (const account of selection.accounts) {
          const blocked = await markCodexQuotaBlocked(
            account,
            new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
              status: 429,
              headers: {
                "Content-Type": "application/json",
                "Retry-After": new Date(now + 60_000).toUTCString(),
              },
            }),
            now,
          );
          assert.equal(blocked.usageLimitReached, true);
          assert.equal(blocked.retryAtMs, expectedRetryAtMs);
        }

        const response = await handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "use fallback after all Codex circuits open" }),
          }),
          {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            requestId,
            startedAtMs: now,
          },
        );

        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-uos-codex-routing-error"), null);
        assert.equal(response.headers.get("x-uos-upstream"), "metered");
        assert.equal(meteredCalls, 1);
        assert.equal(
          kvStore.has(keyToString(["uos_ai", "paid_fallback", "v3", "request", keyId, requestId])),
          true,
        );
      },
    );
  } finally {
    if (previousAuth === undefined) kvStore.delete(authKey);
    else kvStore.set(authKey, previousAuth);
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetCodexAuthCacheForTest();
    resetMeteredModelsCacheForTest();
    if (previousMeteredKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", previousMeteredKey);
  }
});

Deno.test("openai: temporary free GLM cut uses only Surplus without paid fallback", async (t) => {
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const debugKey = keyToString(DEBUG_ROUTING_KEY);
  const previousDebugRouting = kvStore.get(debugKey);
  const healthPrefix = ["uos_ai", "provider_health", "v1", "surplus", "default"] as const;
  const healthKey = keyToString([...healthPrefix, "current"]);
  let removedProviderCalls = 0;

  const clearSurplusHealth = (): void => {
    for (const encodedKey of [...kvStore.keys()]) {
      const key = JSON.parse(encodedKey) as unknown[];
      if (healthPrefix.every((part, index) => key[index] === part)) kvStore.delete(encodedKey);
    }
    resetProviderHealthThrottleForTest();
  };
  const waitForSurplusHealth = async (event: string): Promise<Record<string, unknown>> => {
    for (let attempt = 0; attempt < 100; attempt += 1) {
      const current = kvStore.get(healthKey) as Record<string, unknown> | undefined;
      if (current?.event === event) return current;
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
    const current = kvStore.get(healthKey) as Record<string, unknown> | undefined;
    assert.fail(`Expected Surplus health event ${event}, received ${String(current?.event ?? "missing")}`);
  };

  Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
  Deno.env.set("METERED_API_KEY", "metered-must-not-run");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  setRemovedProviderApiKeyForTest("removed-provider-must-not-run");
  setRemovedProviderTestAdapterForTest({
    fetchResponses: () => {
      removedProviderCalls += 1;
      throw new Error("RemovedProvider must not run for the temporary GLM cut");
    },
    modelFromEvent: () => null,
    isEligibleModel: () => true,
  });
  kvStore.set(debugKey, {
    scenario: "removed_provider_first",
    expires_at_ms: Date.now() + 60_000,
    updated_at_ms: Date.now(),
  });
  resetDebugRoutingCacheForTest();

  try {
    for (
      const routeCase of [
        { route: "responses", requestId: "free-glm-responses", reasoningEffort: "low" },
        { route: "chat", requestId: "free-glm-chat", reasoningEffort: "medium" },
      ] as const
    ) {
      await t.step(
        `${routeCase.route} bypasses catalogs, Codex, RemovedProvider, Metered, and the ledger`,
        async () => {
          clearSurplusHealth();
          const upstreamUrls: string[] = [];
          const dispatchedProviders: string[] = [];
          let upstreamModel: unknown = null;
          let upstreamReasoningEffort: unknown = null;
          let upstreamTextFormat: unknown = null;
          const response = await withFetchMock(
            (url, bodyText, init) => {
              upstreamUrls.push(url);
              assert.equal(url, "https://api.surplusintelligence.ai/v1/responses");
              assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer surplus-test-key");
              const upstreamRequest = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
              upstreamModel = upstreamRequest?.model ?? null;
              upstreamReasoningEffort = (upstreamRequest?.reasoning as Record<string, unknown> | undefined)?.effort ??
                null;
              upstreamTextFormat = (upstreamRequest?.text as Record<string, unknown> | undefined)?.format ?? null;
              return new Response(sseResponse(baseSseChunks()).body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": routeCase.requestId + "-provider",
                },
              });
            },
            () => {
              const context = {
                keyId: "key-" + routeCase.requestId,
                kernelRepo: null,
                kernelOrg: null,
                paidFallbackEnabled: false,
                requestId: routeCase.requestId,
                startedAtMs: Date.now(),
                startedAtMonotonicMs: performance.now(),
                beforeProviderDispatch: (provider: string) => {
                  dispatchedProviders.push(provider);
                  return Promise.resolve();
                },
              };
              return routeCase.route === "responses"
                ? handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                      input: "ping",
                      reasoning: { effort: routeCase.reasoningEffort },
                    }),
                  }),
                  context,
                )
                : handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      reasoning_effort: routeCase.reasoningEffort,
                      response_format: { type: "json_object" },
                    }),
                  }),
                  context,
                );
            },
          );

          assert.equal(response.status, 200);
          assert.equal(response.headers.get("x-uos-upstream"), "surplus");
          await response.text();
          assert.deepEqual(upstreamUrls, ["https://api.surplusintelligence.ai/v1/responses"]);
          assert.deepEqual(dispatchedProviders, ["surplus"]);
          assert.equal(upstreamModel, TEMPORARY_FREE_SURPLUS_TEST_MODEL);
          assert.equal(upstreamReasoningEffort, routeCase.reasoningEffort);
          assert.deepEqual(upstreamTextFormat, routeCase.route === "chat" ? { type: "json_object" } : null);
          assert.ok(!parseWarnings(response.headers.get("x-uos-warning")).includes("response_format_ignored"));
          const telemetry = getResponseTelemetry(response);
          assert.equal(telemetry?.provider, "surplus");
          assert.equal(telemetry?.fallbackReason, null);
          assert.equal(telemetry?.reasoning, routeCase.reasoningEffort);
          assert.equal(telemetry?.providerRequestId, routeCase.requestId + "-provider");
          assert.deepEqual(telemetry?.attemptedProviders, ["surplus"]);
          assert.equal(telemetry?.firstCodexDispatchMs, null);
          assert.equal(telemetry?.firstCodexHeadersMs, null);
          assert.equal(typeof telemetry?.firstProviderDispatchMs, "number");
          assert.equal(typeof telemetry?.firstProviderHeadersMs, "number");
          assert.equal(
            getStoredPaidFallbackRequest("key-" + routeCase.requestId, routeCase.requestId),
            null,
          );
          const health = await waitForSurplusHealth("success");
          assert.equal(health.status, 200);
          assert.equal(health.provider_request_id, routeCase.requestId + "-provider");
        },
      );
    }

    await t.step("ordinary API-key quota rejection happens before Surplus transport", async () => {
      clearSurplusHealth();
      const keyId = "free-glm-quota-key";
      const requestId = "free-glm-quota-request";
      let fetchCalls = 0;
      const response = await withFetchMock(
        () => {
          fetchCalls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                messages: [{ role: "user", content: "ping" }],
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
              beforeProviderDispatch: () =>
                Promise.reject(new ApiKeyQuotaDispatchError("API key quota reservation is unavailable")),
            },
          ),
      );
      assert.equal(response.status, 503);
      assert.equal(response.headers.get("x-uos-upstream"), "surplus");
      assert.equal(fetchCalls, 0);
      assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["surplus"]);
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      assert.equal(kvStore.has(healthKey), false);
    });

    await t.step("tool-bearing requests fail before every provider and paid ledger", async () => {
      clearSurplusHealth();
      for (const route of ["responses", "chat"] as const) {
        const keyId = `free-glm-tools-${route}-key`;
        const requestId = `free-glm-tools-${route}-request`;
        const dispatchedProviders: string[] = [];
        let fetchCalls = 0;
        const response = await withFetchMock(
          () => {
            fetchCalls += 1;
            throw new Error("tool-bearing GLM requests must not reach a provider");
          },
          () => {
            const context = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
              beforeProviderDispatch: (provider: string) => {
                dispatchedProviders.push(provider);
                return Promise.resolve();
              },
            };
            const tool = {
              type: "function",
              name: "inspect_workspace",
              description: "Inspect the workspace.",
              parameters: { type: "object", properties: {}, additionalProperties: false },
            };
            return route === "responses"
              ? handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                    input: "inspect the workspace",
                    tools: [tool],
                  }),
                }),
                context,
              )
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                    messages: [{ role: "user", content: "inspect the workspace" }],
                    tools: [{ type: "function", function: tool }],
                  }),
                }),
                context,
              );
          },
        );

        assert.equal(response.status, 400, route);
        const payload = await response.json() as {
          error?: { code?: string; param?: string };
        };
        assert.equal(payload.error?.code, "unsupported_model_capability", route);
        assert.equal(payload.error?.param, "tools", route);
        assert.equal(fetchCalls, 0, route);
        assert.deepEqual(dispatchedProviders, [], route);
        assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, [], route);
        assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null, route);
        assert.equal(kvStore.has(healthKey), false, route);
      }
    });

    await t.step("Surplus 429 remains quota health and never falls through", async () => {
      clearSurplusHealth();
      const keyId = "free-glm-provider-quota-key";
      const requestId = "free-glm-provider-quota-request";
      const upstreamUrls: string[] = [];
      const response = await withFetchMock(
        (url) => {
          upstreamUrls.push(url);
          return new Response(
            JSON.stringify({
              error: { message: "temporary provider quota", type: "rate_limit_error", code: "provider_quota" },
            }),
            {
              status: 429,
              headers: {
                "Content-Type": "application/json",
                "X-Oneapi-Request-Id": "free-glm-provider-429",
              },
            },
          );
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: TEMPORARY_FREE_SURPLUS_TEST_MODEL, input: "ping" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 429);
      assert.equal(response.headers.get("x-uos-upstream"), "surplus");
      await response.text();
      assert.deepEqual(upstreamUrls, ["https://api.surplusintelligence.ai/v1/responses"]);
      assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["surplus"]);
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      const health = await waitForSurplusHealth("quota_exhausted");
      assert.equal(health.status, 429);
      assert.equal(health.provider_request_id, "free-glm-provider-429");
    });

    await t.step("failed Surplus terminal marks provider health without a paid ledger row", async () => {
      clearSurplusHealth();
      const keyId = "free-glm-terminal-key";
      const requestId = "free-glm-terminal-request";
      const response = await withFetchMock(
        (url) => {
          assert.equal(url, "https://api.surplusintelligence.ai/v1/responses");
          return new Response(
            sseResponse([
              "data: " + JSON.stringify({
                type: "response.failed",
                response: {
                  id: "free-glm-failed-response",
                  status: "failed",
                  model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                  output: [],
                  error: { type: "server_error", code: "provider_error", message: "provider failed" },
                },
              }) + "\n\n",
            ]).body,
            {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "free-glm-failed-provider",
              },
            },
          );
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: TEMPORARY_FREE_SURPLUS_TEST_MODEL, input: "ping" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.headers.get("x-uos-upstream"), "surplus");
      await response.text();
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      const health = await waitForSurplusHealth("upstream_error");
      assert.equal(health.status, null);
      assert.equal(health.provider_request_id, "free-glm-failed-provider");
    });

    await t.step("contentless Surplus Chat completion marks provider health as failed", async () => {
      clearSurplusHealth();
      const keyId = "free-glm-empty-chat-key";
      const requestId = "free-glm-empty-chat-request";
      const response = await withFetchMock(
        (url) => {
          assert.equal(url, "https://api.surplusintelligence.ai/v1/responses");
          return new Response(
            sseResponse([
              `data: ${
                JSON.stringify({
                  type: "response.completed",
                  response: {
                    id: "free-glm-empty-response",
                    status: "completed",
                    model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                    output: [],
                    usage: { input_tokens: 1642, output_tokens: 2048, total_tokens: 3690 },
                  },
                })
              }\n\n`,
            ]).body,
            {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "free-glm-empty-provider",
              },
            },
          );
        },
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                max_completion_tokens: 2048,
                messages: [{ role: "user", content: "contentless GLM response" }],
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 502);
      assert.equal(response.headers.get("x-uos-upstream"), "surplus");
      const payload = await response.json() as { error?: { code?: unknown } };
      assert.equal(payload.error?.code, "empty_upstream_completion");
      assert.equal(getResponseTelemetry(response)?.completed, false);
      assert.equal(getResponseTelemetry(response)?.failureKind, "empty_upstream_completion");
      assert.equal(getResponseTelemetry(response)?.semanticOutputObserved, false);
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      const health = await waitForSurplusHealth("upstream_error");
      assert.equal(health.status, null);
      assert.equal(health.provider_request_id, "free-glm-empty-provider");
    });

    await t.step("client cancellation after headers does not mark Surplus degraded", async () => {
      clearSurplusHealth();
      const keyId = "free-glm-cancel-key";
      const requestId = "free-glm-cancel-request";
      let upstreamCancelled = 0;
      const response = await withFetchMock(
        (url) => {
          assert.equal(url, "https://api.surplusintelligence.ai/v1/responses");
          const body = new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(TEXT_ENCODER.encode(
                "data: " + JSON.stringify({
                  type: "response.created",
                  response: { id: "free-glm-cancel-response", model: TEMPORARY_FREE_SURPLUS_TEST_MODEL },
                }) + "\n\n",
              ));
              controller.enqueue(TEXT_ENCODER.encode(
                "data: " + JSON.stringify({ type: "response.output_text.delta", delta: "started" }) + "\n\n",
              ));
            },
            cancel() {
              upstreamCancelled += 1;
            },
          });
          return new Response(body, {
            status: 200,
            headers: {
              "Content-Type": "text/event-stream",
              "X-Oneapi-Request-Id": "free-glm-cancel-provider",
            },
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: TEMPORARY_FREE_SURPLUS_TEST_MODEL,
                input: "ping",
                stream: true,
              }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId,
              startedAtMs: Date.now(),
            },
          ),
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "surplus");
      await response.body?.cancel("client stopped");
      assert.equal(upstreamCancelled, 1);
      assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
      const health = await waitForSurplusHealth("reachable");
      assert.equal(health.status, 200);
      await new Promise((resolve) => setTimeout(resolve, 5));
      assert.equal((kvStore.get(healthKey) as Record<string, unknown>).event, "reachable");
    });

    assert.equal(removedProviderCalls, 0);
  } finally {
    clearSurplusHealth();
    setRemovedProviderTestAdapterForTest(null);
    setRemovedProviderApiKeyForTest(undefined);
    if (previousDebugRouting === undefined) kvStore.delete(debugKey);
    else kvStore.set(debugKey, previousDebugRouting);
    resetDebugRoutingCacheForTest();
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
  }
});

Deno.test("openai: DeepSeek Flash tool requests route directly to catalog-proven Surplus", async () => {
  const model = "deepseek-v4-flash";
  const keyId = "dynamic-deepseek-surplus-tools";
  const requestId = `request-${keyId}`;
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const originalDateNow = Date.now;
  let nowMs = originalDateNow();
  Date.now = () => nowMs;
  Deno.env.set("METERED_API_KEY", "metered-test-key");
  Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  seedPaidFallbackKey(keyId, {
    limitMicrocredits: -1,
    modelIds: [DEFAULT_TEST_MODEL],
  });
  const tools = [{
    type: "function",
    name: "inspect_workspace",
    description: "Inspect the workspace before continuing.",
    parameters: { type: "object", properties: {}, additionalProperties: false },
    strict: false,
  }];

  try {
    // The explicit inference request must refresh stale non-null catalogs
    // before it decides that this paid-only model belongs on Codex.
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: "previous-metered-model",
            supported_endpoint_types: ["openai-response"],
          }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: "previous-surplus-model",
            provider: "Surplus",
            pricing: { prompt: 0.000001, completion: 0.000003 },
          }],
        })),
    });
    nowMs += Math.max(METERED_MODELS_CACHE_TTL_MS, SURPLUS_MODELS_CACHE_TTL_MS) + 1;
    setMeteredModelsFetchForTest((input, init) => globalThis.fetch(input, init));

    let meteredCatalogCalls = 0;
    let surplusCatalogCalls = 0;
    let codexCalls = 0;
    let surplusCalls = 0;
    let forwardedBody: Record<string, unknown> | null = null;
    const response = await withFetchMock(
      (url, bodyText) => {
        if (url === "https://api.openlux.ai/v1/models") {
          meteredCatalogCalls += 1;
          return Response.json({
            data: [{
              id: "previous-metered-model",
              supported_endpoint_types: ["openai-response"],
            }],
          });
        }
        if (url === "https://api.surplusintelligence.ai/v1/models") {
          surplusCatalogCalls += 1;
          return Response.json({
            data: [{
              id: model,
              provider: "DeepSeek",
              supported_parameters: ["tools", "tool_choice", "reasoning"],
              supported_features: ["streaming", "tools", "reasoning"],
              pricing: { prompt: 0.000001, completion: 0.000003 },
            }],
          });
        }
        if (url === "https://chatgpt.com/backend-api/codex/responses") {
          codexCalls += 1;
          throw new Error("dynamic DeepSeek requests must not reach Codex");
        }
        if (url === "https://api.surplusintelligence.ai/v1/responses") {
          surplusCalls += 1;
          forwardedBody = JSON.parse(String(bodyText)) as Record<string, unknown>;
          return sseResponse(baseSseChunks());
        }
        throw new Error(`Unexpected upstream request in DeepSeek direct-routing test: ${url}`);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model,
              input: "inspect the workspace",
              tools,
              tool_choice: "auto",
              parallel_tool_calls: true,
              reasoning: { effort: "max" },
              stream: true,
            }),
          }),
          {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId,
            startedAtMs: Date.now(),
            startedAtMonotonicMs: performance.now(),
          },
        ),
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "surplus");
    assert.equal(meteredCatalogCalls, 1);
    assert.equal(surplusCatalogCalls, 1);
    assert.equal(codexCalls, 0);
    assert.equal(surplusCalls, 1);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["surplus"]);
    assert.equal(getResponseTelemetry(response)?.fallbackReason, "dynamic_paid_model");
    assert.equal(typeof getResponseTelemetry(response)?.firstProviderDispatchMs, "number");
    assert.equal(typeof getResponseTelemetry(response)?.firstProviderHeadersMs, "number");
    assert.equal(getResponseTelemetry(response)?.firstCodexDispatchMs, null);
    assert.equal(getResponseTelemetry(response)?.firstCodexHeadersMs, null);
    assert.deepEqual(forwardedBody, {
      model,
      input: [{
        type: "message",
        role: "user",
        content: [{ type: "input_text", text: "inspect the workspace" }],
      }],
      store: false,
      stream: true,
      reasoning: { effort: "max" },
      tools,
      tool_choice: "auto",
    });
    await response.text();
    const stored = await waitForPaidFallbackTerminal(keyId, requestId, "completed");
    assert.equal(stored.provider, "surplus");
  } finally {
    Date.now = originalDateNow;
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetMeteredModelsCacheForTest();
    setMeteredModelsFetchForTest(null);
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: direct paid admission failures do not enter removed-provider recovery", async () => {
  const model = "deepseek-v4-flash";
  const keyId = "dynamic-deepseek-admission-stop";
  const requestId = `request-${keyId}`;
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.delete("METERED_API_KEY");
  Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  seedPaidFallbackKey(keyId, { limitMicrocredits: -1 });

  let removedProviderCalls = 0;
  setRemovedProviderApiKeyForTest("removed-provider-test-key");
  setRemovedProviderTestAdapterForTest({
    fetchResponses: async (_body, options) => {
      removedProviderCalls += 1;
      await options.beforeDispatch?.();
      options.timing?.onDispatch?.();
      options.timing?.onHeaders?.();
      return { response: sseResponse(baseSseChunks()) };
    },
    modelFromEvent: () => model,
    isEligibleModel: (candidate) => candidate === model,
  });

  try {
    // Endpoint support without complete pricing proves that the model is paid
    // only, but it must fail admission before any provider transport.
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: model, provider: "DeepSeek" }],
        })),
    });

    let upstreamCalls = 0;
    const response = await withFetchMock(
      (url) => {
        upstreamCalls += 1;
        throw new Error(`Admission failure must not reach an upstream: ${url}`);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model, input: "do not recover", stream: true }),
          }),
          {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId,
            startedAtMs: Date.now(),
          },
        ),
    );

    assert.equal(response.status, 503);
    const payload = await response.json() as { error?: { code?: string } };
    assert.equal(payload.error?.code, "paid_provider_unconfigured");
    assert.equal(upstreamCalls, 0);
    assert.equal(removedProviderCalls, 0);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, []);
    assert.equal(getResponseTelemetry(response)?.fallbackReason, "dynamic_paid_model");
    assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
  } finally {
    setRemovedProviderTestAdapterForTest(null);
    setRemovedProviderApiKeyForTest(undefined);
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: unknown paid-model routing honors catalog refresh backoff", async () => {
  const model = "deepseek-v4-flash";
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const originalDateNow = Date.now;
  let nowMs = originalDateNow();
  Date.now = () => nowMs;
  Deno.env.set("METERED_API_KEY", "metered-test-key");
  Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();

  try {
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: "previous-metered-model", supported_endpoint_types: ["openai-response"] }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: model,
            provider: "DeepSeek",
            pricing: { prompt: 0.000001, completion: 0.000003 },
          }],
        })),
    });
    nowMs += Math.max(METERED_MODELS_CACHE_TTL_MS, SURPLUS_MODELS_CACHE_TTL_MS) + 1;
    setMeteredModelsFetchForTest((input, init) => globalThis.fetch(input, init));

    let meteredCatalogCalls = 0;
    let surplusCatalogCalls = 0;
    let inferenceCalls = 0;
    await withFetchMock(
      (url) => {
        if (url === "https://api.openlux.ai/v1/models") {
          meteredCatalogCalls += 1;
          return new Response("catalog unavailable", { status: 503 });
        }
        if (url === "https://api.surplusintelligence.ai/v1/models") {
          surplusCatalogCalls += 1;
          return new Response("catalog unavailable", { status: 503 });
        }
        inferenceCalls += 1;
        throw new Error(`Disabled direct routing must not reach inference: ${url}`);
      },
      async () => {
        for (let attempt = 0; attempt < 2; attempt += 1) {
          const response = await handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model, input: "respect discovery backoff" }),
            }),
            {
              keyId: `catalog-backoff-${attempt}`,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: false,
              requestId: `request-catalog-backoff-${attempt}`,
              startedAtMs: Date.now(),
            },
          );
          assert.equal(response.status, 403);
          const payload = await response.json() as { error?: { code?: string } };
          assert.equal(payload.error?.code, "paid_fallback_disabled");
        }
      },
    );

    assert.equal(meteredCatalogCalls, 1);
    assert.equal(surplusCatalogCalls, 1);
    assert.equal(inferenceCalls, 0);
  } finally {
    Date.now = originalDateNow;
    resetMeteredModelsCacheForTest();
    setMeteredModelsFetchForTest(null);
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: dynamic tool requests reject unverified Surplus capability before transport", async () => {
  const model = "deepseek-v4-flash";
  const keyId = "dynamic-deepseek-unverified-tools";
  const requestId = `request-${keyId}`;
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.delete("METERED_API_KEY");
  Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
  resetMeteredModelsCacheForTest();
  resetSurplusModelsCacheForTest();
  seedPaidFallbackKey(keyId, { limitMicrocredits: -1 });

  try {
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: model,
            provider: "DeepSeek",
            supported_parameters: ["tools"],
            supported_features: ["tools"],
            pricing: { prompt: 0.000001, completion: 0.000003 },
          }],
        })),
    });

    let upstreamCalls = 0;
    const response = await withFetchMock(
      (url) => {
        upstreamCalls += 1;
        throw new Error(`Unverified dynamic tool request must not reach an upstream: ${url}`);
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model,
              input: "inspect the workspace",
              tools: [{
                type: "function",
                name: "inspect_workspace",
                description: "Inspect the workspace before continuing.",
                parameters: { type: "object", properties: {}, additionalProperties: false },
              }],
              reasoning: { effort: "max" },
              stream: true,
            }),
          }),
          {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            paidFallbackEnabled: true,
            requestId,
            startedAtMs: Date.now(),
          },
        ),
    );

    assert.equal(response.status, 400);
    const payload = await response.json() as { error?: { code?: string; param?: string } };
    assert.equal(payload.error?.code, "model_tool_calling_unsupported");
    assert.equal(payload.error?.param, "tools");
    assert.equal(upstreamCalls, 0);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, []);
    assert.equal(getResponseTelemetry(response)?.fallbackReason, "dynamic_paid_model");
    assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
  } finally {
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: tool-bearing paid fallback skips Surplus without capability evidence", async () => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const keyId = "fallback-tools-skip-unverified-surplus";
  const requestId = `request-${keyId}`;
  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response"] }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: DEFAULT_TEST_MODEL,
            pricing: { prompt: 0.000001, completion: 0.000003 },
          }],
        })),
    });
    seedPaidFallbackKey(keyId);
    let surplusCalls = 0;
    let meteredCalls = 0;

    const response = await withFetchMock(
      (url) => {
        if (url === "https://api.surplusintelligence.ai/v1/responses") {
          surplusCalls += 1;
          throw new Error("unverified Surplus tool transport must not start");
        }
        if (url === "https://api.openlux.ai/v1/responses") {
          meteredCalls += 1;
          return sseResponse(baseSseChunks());
        }
        return authoritativeCodexQuotaResponse();
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              input: "inspect the workspace",
              tools: [{
                type: "function",
                name: "inspect_workspace",
                description: "Inspect the workspace before continuing.",
                parameters: { type: "object", properties: {}, additionalProperties: false },
              }],
            }),
          }),
          { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
        ),
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "metered");
    assert.equal(surplusCalls, 0);
    assert.equal(meteredCalls, 1);
    const stored = await waitForPaidFallbackTerminal(keyId, requestId, "completed");
    assert.equal(stored.provider, "metered");
  } finally {
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: Codex model-unsupported responses never enter paid fallback", async (t) => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  const routeCases = [
    { route: "responses", keyId: "fallback-codex-model-unsupported-responses" },
    { route: "chat", keyId: "fallback-codex-model-unsupported-chat" },
  ] as const;
  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai", "openai-response"] }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, pricing: { prompt: 0.000001, completion: 0.000003 } }],
        })),
    });
    for (const routeCase of routeCases) {
      await t.step(`${routeCase.route} returns the primary 400 without paid exposure`, async () => {
        const { keyId } = routeCase;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        let codexCalls = 0;
        let surplusCalls = 0;
        let meteredCalls = 0;

        const response = await withFetchMock(
          (url) => {
            if (url === "https://chatgpt.com/backend-api/codex/responses") {
              codexCalls += 1;
              return new Response(
                JSON.stringify({
                  message:
                    "The 'gpt-5-fixture-default' model is not supported when using Codex with a ChatGPT account.",
                  type: "invalid_request_error",
                  code: "upstream_error",
                }),
                { status: 400, headers: { "Content-Type": "application/json" } },
              );
            }
            if (url === "https://api.surplusintelligence.ai/v1/responses") {
              surplusCalls += 1;
              throw new Error("model-support errors must not dispatch to Surplus");
            }
            if (url === "https://api.openlux.ai/v1/responses") {
              meteredCalls += 1;
              throw new Error("model-support errors must not dispatch to OpenLux");
            }
            throw new Error(`Unexpected upstream dispatch in Codex model unsupported test: ${url}`);
          },
          () => {
            const usageContext = {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              paidFallbackEnabled: true,
              requestId,
              startedAtMs: Date.now(),
            };
            return routeCase.route === "responses"
              ? handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    input: "inspect the workspace",
                    tools: [{
                      type: "function",
                      name: "inspect_workspace",
                      description: "Inspect the workspace before continuing.",
                      parameters: { type: "object", properties: {}, additionalProperties: false },
                    }],
                    tool_choice: "none",
                  }),
                }),
                usageContext,
              )
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "inspect the workspace" }],
                  }),
                }),
                usageContext,
              );
          },
        );

        assert.equal(response.status, 400);
        assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
        assert.equal(getResponseTelemetry(response)?.provider, "chatgpt_codex");
        assert.equal(getResponseTelemetry(response)?.fallbackReason, null);
        assert.equal(codexCalls, 1);
        assert.equal(surplusCalls, 0);
        assert.equal(meteredCalls, 0);
        const payload = await response.json() as { error?: Record<string, unknown> };
        assert.equal(payload.error?.code, "upstream_error");
        assert.equal(
          payload.error?.message,
          "The 'gpt-5-fixture-default' model is not supported when using Codex with a ChatGPT account.",
        );
        assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
        const keyRecord = kvStore.get(keyToString(["ubq_ai", "api_keys", "id", keyId])) as {
          usage_reset_at_ms: number;
          paid_fallback_spent_microcredits: number;
          paid_fallback_reserved_microcredits: number;
          paid_fallback_reservation_request_id: string | null;
        };
        assert.equal(keyRecord.paid_fallback_spent_microcredits, 0);
        assert.equal(keyRecord.paid_fallback_reserved_microcredits, 0);
        assert.equal(keyRecord.paid_fallback_reservation_request_id, null);
        assert.equal(
          kvStore.get(
            keyToString(["uos_ai", "paid_fallback", "v3", "window", keyId, keyRecord.usage_reset_at_ms]),
          ),
          undefined,
        );
      });
    }
  } finally {
    for (const { keyId } of routeCases) {
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "id", keyId]));
      kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", `hash-${keyId}`]));
    }
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: inter-provider abort and quota rejection retain the responding provider request ID", async () => {
  const originalMeteredApiKey = Deno.env.get("METERED_API_KEY");
  const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
  try {
    Deno.env.set("METERED_API_KEY", "metered-test-key");
    Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    await fetchMeteredModels({
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response"] }],
        })),
    });
    await fetchSurplusModels({
      apiKey: "surplus-test-key",
      force: true,
      fetcher: () =>
        Promise.resolve(Response.json({
          data: [{
            id: DEFAULT_TEST_MODEL,
            pricing: { prompt: 0.000001, completion: 0.000003 },
          }],
        })),
    });

    const abortKeyId = "fallback-inter-provider-abort";
    const abortRequestId = `request-${abortKeyId}`;
    seedPaidFallbackKey(abortKeyId);
    const controller = new AbortController();
    let abortMeteredCalls = 0;
    const abortedResponse = await withFetchMock(
      (url) => {
        if (url === "https://api.surplusintelligence.ai/v1/responses") {
          return new Response(
            new ReadableStream<Uint8Array>({
              start(streamController) {
                streamController.enqueue(TEXT_ENCODER.encode("provider one limited"));
              },
              cancel() {
                controller.abort(new DOMException("client disconnected", "AbortError"));
              },
            }),
            {
              status: 429,
              headers: { "X-Oneapi-Request-Id": "provider-1-abort-id" },
            },
          );
        }
        if (url === "https://api.openlux.ai/v1/responses") {
          abortMeteredCalls += 1;
          return sseResponse(baseSseChunks());
        }
        return authoritativeCodexQuotaResponse();
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
            keyId: abortKeyId,
            kernelRepo: null,
            kernelOrg: null,
            requestId: abortRequestId,
            startedAtMs: Date.now(),
          },
        ),
    );
    assert.equal(abortedResponse.status, 499);
    assert.equal(abortMeteredCalls, 0);
    const abortedTelemetry = getResponseTelemetry(abortedResponse);
    assert.equal(abortedTelemetry?.provider, "surplus");
    assert.equal(abortedTelemetry?.providerRequestId, "provider-1-abort-id");
    assert.equal(abortedResponse.headers.get("x-uos-upstream"), "surplus");
    const aborted = await waitForPaidFallbackTerminal(abortKeyId, abortRequestId, "ambiguous");
    assert.equal(aborted.dispatch_state, "dispatched");
    assert.equal(aborted.provider, "surplus");
    assert.equal(aborted.provider_request_id, "provider-1-abort-id");
    assert.equal(aborted.billing_state, "pending");

    const quotaKeyId = "fallback-provider-two-quota";
    const quotaRequestId = `request-${quotaKeyId}`;
    seedPaidFallbackKey(quotaKeyId);
    let quotaMeteredCalls = 0;
    const quotaResponse = await withFetchMock(
      (url) => {
        if (url === "https://api.surplusintelligence.ai/v1/responses") {
          return new Response("provider one limited", {
            status: 429,
            headers: { "X-Oneapi-Request-Id": "provider-1-quota-id" },
          });
        }
        if (url === "https://api.openlux.ai/v1/responses") {
          quotaMeteredCalls += 1;
          return sseResponse(baseSseChunks());
        }
        return authoritativeCodexQuotaResponse();
      },
      () =>
        handleResponses(
          new Request("https://ai.ubq.fi/v1/responses", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
          }),
          {
            keyId: quotaKeyId,
            kernelRepo: null,
            kernelOrg: null,
            requestId: quotaRequestId,
            startedAtMs: Date.now(),
            beforeProviderDispatch: (provider) =>
              provider === "metered"
                ? Promise.reject(new ApiKeyQuotaDispatchError("API key quota reservation is unavailable"))
                : Promise.resolve(),
          },
        ),
    );
    assert.equal(quotaResponse.status, 503);
    assert.equal(quotaMeteredCalls, 0);
    const quotaTelemetry = getResponseTelemetry(quotaResponse);
    assert.equal(quotaTelemetry?.provider, "surplus");
    assert.equal(quotaTelemetry?.providerRequestId, "provider-1-quota-id");
    assert.equal(quotaResponse.headers.get("x-uos-upstream"), "surplus");
    const quotaRejected = await waitForPaidFallbackTerminal(quotaKeyId, quotaRequestId, "ambiguous");
    assert.equal(quotaRejected.dispatch_state, "dispatched");
    assert.equal(quotaRejected.provider, "surplus");
    assert.equal(quotaRejected.provider_request_id, "provider-1-quota-id");
    assert.equal(quotaRejected.billing_state, "pending");
  } finally {
    resetMeteredModelsCacheForTest();
    resetSurplusModelsCacheForTest();
    if (originalMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalMeteredApiKey);
    if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
  }
});

Deno.test("openai: Metered paid fallback routing matrix", async (t) => {
  const originalApiKey = Deno.env.get("METERED_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-test-key");
  try {
    await t.step("already-loaded disabled policy bypasses paid fallback reservation", async () => {
      const keyId = "fallback-policy-bypass";
      seedPaidFallbackKey(keyId, { enabled: true });
      let calls = 0;
      const response = await withFetchMock(
        () => {
          calls += 1;
          return authoritativeCodexQuotaResponse();
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
      assert.equal(calls, 1);
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
            return authoritativeCodexQuotaResponse();
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
        assert.equal(calls, 1);
        assert.deepEqual(await response.json(), {
          error: {
            message: "Primary limited",
            type: "usage_limit_reached",
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
            return authoritativeCodexQuotaResponse();
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
        assert.match(response.headers.get("Retry-After") ?? "", / GMT$/);
        assert.equal(calls, 1);
      } finally {
        atomicCommitFailure = null;
      }
    });

    await t.step("primary 402, errors, and network failures other than 429 never dispatch Metered", async () => {
      for (const scenario of ["http_402", "http_500", "network"] as const) {
        const keyId = `fallback-${scenario}`;
        seedPaidFallbackKey(keyId);
        let calls = 0;
        const response = await withFetchMock(
          () => {
            calls += 1;
            if (scenario === "network") throw new TypeError("primary network unavailable");
            return new Response(JSON.stringify({ error: { message: "Primary failed" } }), {
              status: scenario === "http_402" ? 402 : 500,
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
        assert.equal(response.status, scenario === "http_402" ? 402 : scenario === "http_500" ? 500 : 502);
        assert.equal(calls, 1);
      }
    });

    await t.step("primary 401 and 403 fail closed without paid dispatch", async () => {
      const debugKey = keyToString(DEBUG_ROUTING_KEY);
      const previousDebugRouting = kvStore.get(debugKey);
      try {
        for (const status of [401, 403] as const) {
          const keyId = `fallback-primary-${status}`;
          const requestId = `request-${keyId}`;
          seedPaidFallbackKey(keyId);
          kvStore.set(debugKey, {
            scenario: `codex_${status}`,
            expires_at_ms: Date.now() + 60_000,
            updated_at_ms: Date.now(),
          });
          resetDebugRoutingCacheForTest();
          let paidCalls = 0;
          const response = await withFetchMock(
            () => {
              paidCalls += 1;
              throw new Error(`primary ${status} must not dispatch paid inference`);
            },
            () =>
              handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "fail closed" }),
                }),
                { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
              ),
          );

          assert.equal(response.status, status);
          assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
          assert.equal(getResponseTelemetry(response)?.fallbackReason, null);
          assert.equal(paidCalls, 0);
          assert.equal(getStoredPaidFallbackRequest(keyId, requestId), null);
        }
        kvStore.set(debugKey, {
          scenario: "normal",
          expires_at_ms: null,
          updated_at_ms: Date.now(),
        });
        resetDebugRoutingCacheForTest();
      } finally {
        if (previousDebugRouting === undefined) kvStore.delete(debugKey);
        else kvStore.set(debugKey, previousDebugRouting);
        resetDebugRoutingCacheForTest();
      }
    });

    await t.step("a legacy timeout marker does not authorize paid fallback", async () => {
      const keyId = "fallback-upstream-degraded";
      const requestId = "request-fallback-upstream-degraded";
      seedPaidFallbackKey(keyId);
      let codexCalls = 0;
      let meteredCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
            throw new Error("a transient Codex failure must not dispatch to a paid provider");
          }
          codexCalls += 1;
          return new Response(JSON.stringify({ error: { message: "transient upstream failure" } }), {
            status: 503,
            headers: { "Content-Type": "application/json" },
          });
        },
        async () => {
          const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
          const selected = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
          assert.equal(selected.kind, "eligible");
          if (selected.kind !== "eligible") throw new Error("expected an eligible timeout fixture account");
          await markCodexUpstreamTimeout(selected.accounts[0]!);
          return await handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "timeout circuit" }),
            }),
            {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
            },
          );
        },
      );

      assert.equal(response.status, 503);
      assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
      assert.equal(getResponseTelemetry(response)?.fallbackReason, null);
      assert.equal(codexCalls, 1);
      assert.equal(meteredCalls, 0);
      await response.text();
    });

    await t.step("cancellation before fallback admission creates no paid exposure", async () => {
      const keyId = "fallback-cancel-before-dispatch";
      const requestId = "request-fallback-cancel-before-dispatch";
      seedPaidFallbackKey(keyId);
      const controller = new AbortController();
      let codexCalls = 0;
      let meteredCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.openlux.ai/v1/responses") {
            meteredCalls += 1;
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
      assert.equal(response.status, 499);
      const cancellation = await response.json() as { error?: { type?: unknown; code?: unknown; param?: unknown } };
      assert.equal(cancellation.error?.type, "server_error");
      assert.equal(cancellation.error?.code, "request_cancelled");
      assert.equal(cancellation.error?.param, null);
      assert.equal(codexCalls, 1);
      assert.equal(meteredCalls, 0);
      assert.equal(getResponseTelemetry(response)?.provider, "chatgpt_codex");
      assert.equal(getResponseTelemetry(response)?.streamTerminalType, "cancelled");
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

    await t.step("Responses strips only Codex-incompatible controls before Metered fallback", async () => {
      const keyId = "fallback-responses-success";
      seedPaidFallbackKey(keyId);
      const bodies: Record<string, unknown>[] = [];
      const urls: string[] = [];
      const response = await withFetchMock(
        (url, bodyText, init) => {
          urls.push(url);
          if (bodyText) bodies.push(JSON.parse(bodyText) as Record<string, unknown>);
          if (url === "https://api.openlux.ai/v1/responses") {
            const stored = getStoredPaidFallbackRequest(
              keyId,
              "request-fallback-responses-success",
            );
            assert.equal(stored?.dispatch_state, "dispatched");
            assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer metered-test-key");
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "metered-responses-request",
              },
            });
          }
          return authoritativeCodexQuotaResponse({
            "x-uos-warning": "codex_quota_temporarily_exceeded",
          });
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                input: [{
                  type: "message",
                  role: "user",
                  content: [{
                    type: "input_text",
                    text: "stable fallback prefix",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  }],
                }],
                max_output_tokens: 64,
                prompt_cache_key: "fallback-cache-key",
                prompt_cache_options: { mode: "explicit", ttl: "30m" },
                prompt_cache_retention: "24h",
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
      assert.equal(response.headers.get("x-uos-upstream"), "metered");
      assert.equal(response.headers.get("x-uos-warning"), null);
      assert.equal(getResponseTelemetry(response)?.quotaUsedPercent, 0);
      assert.equal(getResponseTelemetry(response)?.fallbackReason, "primary_quota_blocked");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://api.openlux.ai/v1/responses",
      ]);
      assert.equal(bodies.length, 2);
      assert.equal(bodies[0].prompt_cache_key, "fallback-cache-key");
      assert.equal("max_output_tokens" in bodies[0], false);
      assert.equal("prompt_cache_options" in bodies[0], false);
      assert.equal("prompt_cache_retention" in bodies[0], false);
      const codexInput = bodies[0].input as Array<Record<string, unknown>>;
      const codexContent = codexInput[0]?.content as Array<Record<string, unknown>>;
      assert.equal("prompt_cache_breakpoint" in codexContent[0]!, false);
      assert.equal(bodies[1].max_output_tokens, 64);
      assert.equal(bodies[1].prompt_cache_key, "fallback-cache-key");
      assert.deepEqual(bodies[1].prompt_cache_options, { mode: "explicit", ttl: "30m" });
      assert.equal(bodies[1].prompt_cache_retention, "24h");
      const meteredInput = bodies[1].input as Array<Record<string, unknown>>;
      const meteredContent = meteredInput[0]?.content as Array<Record<string, unknown>>;
      assert.deepEqual(meteredContent[0]?.prompt_cache_breakpoint, { mode: "explicit" });
      assert.deepEqual(bodies[1].reasoning, { effort: "max" });

      const recordedAnalyticsEvents: Parameters<typeof recordPromptCacheAnalytics>[0][] = [];
      await withTerminalRequestLog(response, {
        route: "responses",
        startedAtMonotonicMs: performance.now(),
        requestId: "cache-analytics-metered-fallback",
        recordCacheAnalytics: (event) => {
          recordedAnalyticsEvents.push(event);
          return Promise.resolve({
            status: "ignored" as const,
            reason: "unknown_release" as const,
            bucket_start_at_ms: null,
          });
        },
        recordTelemetry: () =>
          Promise.resolve({
            status: "ignored" as const,
            reason: "unknown_release" as const,
            release: null,
            provider: null,
            route: null,
            model_hash: null,
          }),
      });
      const recordedAnalyticsEvent = recordedAnalyticsEvents[0];
      assert.ok(recordedAnalyticsEvent);
      assert.deepEqual(
        {
          provider: recordedAnalyticsEvent.provider,
          model: recordedAnalyticsEvent.model,
          route: recordedAnalyticsEvent.route,
          promptCacheKeyPresent: recordedAnalyticsEvent.promptCacheKeyPresent,
          promptCacheMode: recordedAnalyticsEvent.promptCacheMode,
          fallbackReason: recordedAnalyticsEvent.fallbackReason,
        },
        {
          provider: "metered",
          model: DEFAULT_TEST_MODEL,
          route: "responses",
          promptCacheKeyPresent: true,
          promptCacheMode: "explicit",
          fallbackReason: "primary_quota_blocked",
        },
      );
      assert.equal("affinityOutcome" in recordedAnalyticsEvent, false);
    });

    await t.step(
      "streaming Responses closes after Metered's terminal event even when its socket stays open",
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
            if (url === "https://api.openlux.ai/v1/responses") {
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
                  "X-Oneapi-Request-Id": "metered-hanging-socket-request",
                },
              });
            }
            if (url === "https://api.openlux.ai/api/log/token") {
              return new Response(JSON.stringify({ success: true, data: [] }), {
                status: 200,
                headers: { "Content-Type": "application/json" },
              });
            }
            return authoritativeCodexQuotaResponse();
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
            assert.equal(response.headers.get("x-uos-upstream"), "metered");
            return await response.text();
          },
        );

        assert.match(responseText, /"type":"response.completed"/);
        assert.doesNotMatch(responseText, /post-terminal/);
        assert.equal(upstreamCancelled, true);
      },
    );

    await t.step("Chat Completions also falls back through Metered Responses once", async () => {
      const keyId = "fallback-chat-success";
      seedPaidFallbackKey(keyId);
      const urls: string[] = [];
      const response = await withFetchMock(
        (url) => {
          urls.push(url);
          if (url === "https://api.openlux.ai/v1/responses") {
            return new Response(sseResponse(baseSseChunks()).body, {
              status: 200,
              headers: {
                "Content-Type": "text/event-stream",
                "X-Oneapi-Request-Id": "metered-chat-request",
              },
            });
          }
          return authoritativeCodexQuotaResponse();
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
      assert.equal(response.headers.get("x-uos-upstream"), "metered");
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://api.openlux.ai/v1/responses",
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
                output: terminalCase.eventType === "response.completed"
                  ? [{
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "terminal output" }],
                  }]
                  : [],
                usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
              },
            };

          await withFetchMock(
            (url) => {
              if (url === "https://api.openlux.ai/v1/responses") {
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
              if (url.startsWith("https://api.openlux.ai/api/log/token?")) {
                return new Response(JSON.stringify({ success: true, data: { items: [] } }), {
                  status: 200,
                  headers: { "Content-Type": "application/json" },
                });
              }
              return authoritativeCodexQuotaResponse();
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

    await t.step("Metered network ambiguity returns an attributed 502 without retrying", async () => {
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
        const promptCacheKey = `fallback-cache-key-${suffix}`;
        const codexSessionHeaders = ["conversation_id", "session-id", "thread-id", "x-client-request-id"] as const;
        seedPaidFallbackKey(keyId);
        let meteredAttempts = 0;
        const codexRequestHeaders: Headers[] = [];
        const paidRequests: Array<Readonly<{ body: Record<string, unknown>; headers: Headers }>> = [];
        await withFetchMock(
          (url, bodyText, init) => {
            const headers = new Headers(init?.headers);
            if (url === "https://api.openlux.ai/v1/responses") {
              meteredAttempts += 1;
              paidRequests.push({
                body: JSON.parse(bodyText ?? "{}") as Record<string, unknown>,
                headers,
              });
              throw new TypeError("network connection reset before response headers");
            }
            codexRequestHeaders.push(headers);
            return authoritativeCodexQuotaResponse();
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
                    prompt_cache_key: promptCacheKey,
                    prompt_cache_options: { mode: "explicit", ttl: "30m" },
                    prompt_cache_retention: "24h",
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
                    prompt_cache_key: promptCacheKey,
                    prompt_cache_options: { mode: "explicit", ttl: "30m" },
                    prompt_cache_retention: "24h",
                  }),
                }),
                context,
              );
            assert.equal(response.status, 502, suffix);
            assert.equal(response.headers.get("x-uos-upstream"), "metered", suffix);
            assert.equal(meteredAttempts, 1, suffix);
            assert.ok(codexRequestHeaders.length > 0, suffix);
            for (const headers of codexRequestHeaders) {
              const sessionIdentity = headers.get("conversation_id");
              assert.ok(sessionIdentity, suffix);
              for (const header of codexSessionHeaders) {
                assert.equal(headers.get(header), sessionIdentity, `${suffix}:${header}`);
              }
            }
            assert.equal(paidRequests.length, 1, suffix);
            const paidRequest = paidRequests[0]!;
            assert.equal(paidRequest.body.prompt_cache_key, promptCacheKey, suffix);
            assert.deepEqual(paidRequest.body.prompt_cache_options, { mode: "explicit", ttl: "30m" }, suffix);
            assert.equal(paidRequest.body.prompt_cache_retention, "24h", suffix);
            for (const header of codexSessionHeaders) {
              assert.equal(paidRequest.headers.has(header), false, `${suffix}:${header}`);
            }
            const payload = await response.json() as {
              error?: { type?: unknown; code?: unknown };
            };
            assert.equal(payload.error?.type, "server_error", suffix);
            assert.equal(payload.error?.code, "metered_upstream_unreachable", suffix);
            const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
            assert.equal(stored.dispatch_state, "dispatched", suffix);
            assert.equal(stored.provider_request_id, null, suffix);
            assert.equal(stored.billing_state, "pending", suffix);
          },
        );
      }
    });

    await t.step("Surplus network ambiguity retains its provider-specific 502 contract", async () => {
      const previousMeteredApiKey = Deno.env.get("METERED_API_KEY");
      const previousSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
      try {
        Deno.env.set("METERED_API_KEY", "metered-test-key");
        Deno.env.set("SURPLUS_API_KEY", "surplus-test-key");
        resetMeteredModelsCacheForTest();
        resetSurplusModelsCacheForTest();
        await fetchMeteredModels({
          force: true,
          fetcher: () =>
            Promise.resolve(Response.json({
              data: [{ id: DEFAULT_TEST_MODEL, supported_endpoint_types: ["openai-response", "openai"] }],
            })),
        });
        await fetchSurplusModels({
          apiKey: "surplus-test-key",
          force: true,
          fetcher: () =>
            Promise.resolve(Response.json({
              data: [{
                id: DEFAULT_TEST_MODEL,
                pricing: { prompt: 0.000001, completion: 0.000003 },
              }],
            })),
        });
        for (
          const routeCase of [
            { route: "responses", stream: false },
            { route: "responses", stream: true },
            { route: "chat", stream: false },
            { route: "chat", stream: true },
          ] as const
        ) {
          const suffix = `${routeCase.route}-${routeCase.stream ? "stream" : "buffered"}`;
          const keyId = `fallback-surplus-network-${suffix}`;
          const requestId = `request-${keyId}`;
          const promptCacheKey = `fallback-cache-key-${suffix}`;
          const codexSessionHeaders = [
            "conversation_id",
            "session-id",
            "thread-id",
            "x-client-request-id",
          ] as const;
          seedPaidFallbackKey(keyId);
          let surplusAttempts = 0;
          let meteredAttempts = 0;
          const codexRequestHeaders: Headers[] = [];
          const paidRequests: Array<Readonly<{ body: Record<string, unknown>; headers: Headers }>> = [];
          await withFetchMock(
            (url, bodyText, init) => {
              const headers = new Headers(init?.headers);
              if (url === "https://api.surplusintelligence.ai/v1/responses") {
                surplusAttempts += 1;
                paidRequests.push({
                  body: JSON.parse(bodyText ?? "{}") as Record<string, unknown>,
                  headers,
                });
                throw new TypeError("network connection reset before response headers");
              }
              if (url === "https://api.openlux.ai/v1/responses") {
                meteredAttempts += 1;
                return sseResponse(baseSseChunks());
              }
              codexRequestHeaders.push(headers);
              return authoritativeCodexQuotaResponse();
            },
            async () => {
              const response = routeCase.route === "responses"
                ? await handleResponses(
                  new Request("https://ai.ubq.fi/v1/responses", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      input: "ping",
                      stream: routeCase.stream,
                      prompt_cache_key: promptCacheKey,
                      prompt_cache_options: { mode: "explicit", ttl: "30m" },
                      prompt_cache_retention: "24h",
                    }),
                  }),
                  { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
                )
                : await handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      messages: [{ role: "user", content: "ping" }],
                      stream: routeCase.stream,
                      prompt_cache_key: promptCacheKey,
                      prompt_cache_options: { mode: "explicit", ttl: "30m" },
                      prompt_cache_retention: "24h",
                    }),
                  }),
                  { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
                );
              assert.equal(response.status, 502, suffix);
              assert.equal(response.headers.get("x-uos-upstream"), "surplus", suffix);
              assert.equal(surplusAttempts, 1, suffix);
              assert.equal(meteredAttempts, 0, suffix);
              assert.ok(codexRequestHeaders.length > 0, suffix);
              for (const headers of codexRequestHeaders) {
                const sessionIdentity = headers.get("conversation_id");
                assert.ok(sessionIdentity, suffix);
                for (const header of codexSessionHeaders) {
                  assert.equal(headers.get(header), sessionIdentity, `${suffix}:${header}`);
                }
              }
              assert.equal(paidRequests.length, 1, suffix);
              const paidRequest = paidRequests[0]!;
              assert.equal(paidRequest.body.prompt_cache_key, promptCacheKey, suffix);
              assert.deepEqual(paidRequest.body.prompt_cache_options, { mode: "explicit", ttl: "30m" }, suffix);
              assert.equal(paidRequest.body.prompt_cache_retention, "24h", suffix);
              for (const header of codexSessionHeaders) {
                assert.equal(paidRequest.headers.has(header), false, `${suffix}:${header}`);
              }
              assert.deepEqual(await response.json(), {
                error: {
                  message:
                    "Surplus upstream request failed: Surplus Responses request could not reach the upstream service.",
                  type: "server_error",
                  code: "surplus_upstream_unreachable",
                  param: null,
                },
              }, suffix);
              const stored = await waitForPaidFallbackTerminal(keyId, requestId, "ambiguous");
              assert.equal(stored.provider, "surplus", suffix);
              assert.equal(stored.billing_state, "pending", suffix);
            },
          );
        }
      } finally {
        resetMeteredModelsCacheForTest();
        resetSurplusModelsCacheForTest();
        if (previousMeteredApiKey === undefined) Deno.env.delete("METERED_API_KEY");
        else Deno.env.set("METERED_API_KEY", previousMeteredApiKey);
        if (previousSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
        else Deno.env.set("SURPLUS_API_KEY", previousSurplusApiKey);
      }
    });

    await t.step("failed RemovedProvider fallback restores primary Codex correlation", async () => {
      const debugKey = keyToString(DEBUG_ROUTING_KEY);
      const previousDebugRouting = kvStore.get(debugKey);
      const originalInfo = console.info;
      const logs: unknown[][] = [];
      setRemovedProviderApiKeyForTest("removed-provider-test-key");
      setRemovedProviderTestAdapterForTest({
        fetchResponses: () => {
          throw new Error("RemovedProvider fallback failed");
        },
        modelFromEvent: () => null,
        isEligibleModel: (model) => model === DEFAULT_TEST_MODEL,
      });
      kvStore.set(debugKey, {
        scenario: "normal",
        expires_at_ms: Date.now() + 60_000,
        updated_at_ms: Date.now(),
      });
      resetDebugRoutingCacheForTest();
      console.info = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          () =>
            new Response(JSON.stringify({ error: { message: "Codex primary failed" } }), {
              status: 500,
              headers: {
                "Content-Type": "application/json",
                "X-Request-Id": "failed-codex-primary-id",
              },
            }),
          () =>
            gatewayHandler(
              new Request("http://localhost/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "preserve primary correlation" }),
              }),
            ),
        );
        assert.equal(response.status, 500);
        assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
        assert.equal(response.headers.get("x-uos-provider-request-id"), "failed-codex-primary-id");
        await response.json();
        for (let attempt = 0; attempt < 100 && logs.length === 0; attempt += 1) {
          await new Promise<void>((resolve) => setTimeout(resolve, 1));
        }
        const terminals = logs
          .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
          .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>);
        assert.equal(terminals.length, 1);
        const terminal = terminals[0]!;
        assert.equal(terminal.provider, "chatgpt_codex");
        assert.equal(terminal.provider_request_id, "failed-codex-primary-id");
        assert.equal(terminal.account_slot, 1);
        assert.equal(typeof terminal.account_cohort_id, "string");
      } finally {
        console.info = originalInfo;
        setRemovedProviderTestAdapterForTest(null);
        setRemovedProviderApiKeyForTest(undefined);
        if (previousDebugRouting === undefined) kvStore.delete(debugKey);
        else kvStore.set(debugKey, previousDebugRouting);
        resetDebugRoutingCacheForTest();
      }
    });

    await t.step("RemovedProvider recovery clears failed Codex request metadata", async () => {
      const debugKey = keyToString(DEBUG_ROUTING_KEY);
      const previousDebugRouting = kvStore.get(debugKey);
      const originalInfo = console.info;
      const logs: unknown[][] = [];
      setRemovedProviderApiKeyForTest("removed-provider-test-key");
      kvStore.set(debugKey, {
        scenario: "removed_provider_first",
        expires_at_ms: Date.now() + 60_000,
        updated_at_ms: Date.now(),
      });
      resetDebugRoutingCacheForTest();
      console.info = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          () =>
            new Response(JSON.stringify({ error: { message: "Codex recovery failed" } }), {
              status: 500,
              headers: {
                "Content-Type": "application/json",
                "X-Request-Id": "failed-codex-request-id",
              },
            }),
          () =>
            gatewayHandler(
              new Request("http://localhost/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "recover through RemovedProvider" }),
              }),
            ),
        );
        assert.equal(response.status, 502);
        assert.equal(response.headers.get("x-uos-upstream"), "removed_provider");
        assert.equal(response.headers.get("x-uos-provider-request-id"), null);
        const terminal = logs
          .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
          .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>)[0];
        assert.ok(terminal);
        assert.equal(terminal.provider, "removed_provider");
        assert.equal(terminal.provider_request_id, null);
        assert.equal(terminal.account_slot, null);
        assert.equal(terminal.account_cohort_id, null);
      } finally {
        console.info = originalInfo;
        setRemovedProviderApiKeyForTest(undefined);
        if (previousDebugRouting === undefined) kvStore.delete(debugKey);
        else kvStore.set(debugKey, previousDebugRouting);
        resetDebugRoutingCacheForTest();
      }
    });

    await t.step("direct Codex failure selects RemovedProvider without failed metadata", async () => {
      const debugKey = keyToString(DEBUG_ROUTING_KEY);
      const previousDebugRouting = kvStore.get(debugKey);
      const originalInfo = console.info;
      const logs: unknown[][] = [];
      let removedProviderBody: Record<string, unknown> | null = null;
      setRemovedProviderApiKeyForTest("removed-provider-test-key");
      setRemovedProviderTestAdapterForTest({
        fetchResponses: async (body, options) => {
          removedProviderBody = structuredClone(body);
          await options.beforeDispatch?.();
          options.timing?.onDispatch?.();
          options.timing?.onHeaders?.();
          return {
            response: sseResponse([
              `data: ${
                JSON.stringify({
                  type: "response.created",
                  response: { id: "resp_removed_provider_direct", model: DEFAULT_TEST_MODEL },
                })
              }\n\n`,
              `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "Recovered" })}\n\n`,
              `data: ${
                JSON.stringify({
                  type: "response.completed",
                  response: {
                    id: "resp_removed_provider_direct",
                    model: DEFAULT_TEST_MODEL,
                    status: "completed",
                    output: [],
                    usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                  },
                })
              }\n\n`,
            ]),
          };
        },
        modelFromEvent: (value) => {
          const response = value.response;
          if (!response || typeof response !== "object" || Array.isArray(response)) return null;
          const model = (response as Record<string, unknown>).model;
          return typeof model === "string" ? model : null;
        },
        isEligibleModel: (model) => model === DEFAULT_TEST_MODEL,
      });
      kvStore.set(debugKey, {
        scenario: "normal",
        expires_at_ms: Date.now() + 60_000,
        updated_at_ms: Date.now(),
      });
      resetDebugRoutingCacheForTest();
      console.info = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          () =>
            new Response(JSON.stringify({ error: { message: "Codex direct failure" } }), {
              status: 500,
              headers: {
                "Content-Type": "application/json",
                "X-Request-Id": "failed-codex-request-id",
              },
            }),
          () =>
            gatewayHandler(
              new Request("http://localhost/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  prompt_cache_key: "removed-provider-cache-key",
                  prompt_cache_options: { mode: "explicit", ttl: "30m" },
                  prompt_cache_retention: "24h",
                  input: [{
                    type: "input_text",
                    text: "recover through RemovedProvider",
                    prompt_cache_breakpoint: { mode: "explicit" },
                  }],
                }),
              }),
            ),
        );
        assert.equal(response.status, 200);
        assert.equal(response.headers.get("x-uos-upstream"), "removed_provider");
        assert.equal(response.headers.get("x-uos-provider-request-id"), null);
        assert.equal(response.headers.get("x-uos-warning"), null);
        assert.ok(removedProviderBody);
        const forwardedBody = removedProviderBody as Record<string, unknown>;
        assert.equal(forwardedBody.prompt_cache_key, "removed-provider-cache-key");
        assert.deepEqual(forwardedBody.prompt_cache_options, { mode: "explicit", ttl: "30m" });
        assert.equal(forwardedBody.prompt_cache_retention, "24h");
        const forwardedInput = forwardedBody.input as Array<Record<string, unknown>>;
        const forwardedContent = forwardedInput[0]?.content as Array<Record<string, unknown>>;
        assert.deepEqual(forwardedContent[0]?.prompt_cache_breakpoint, { mode: "explicit" });
        await response.text();
        for (let attempt = 0; attempt < 100 && logs.length === 0; attempt += 1) {
          await new Promise<void>((resolve) => setTimeout(resolve, 1));
        }
        const terminals = logs
          .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
          .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>);
        assert.equal(terminals.length, 1);
        const terminal = terminals[0]!;
        assert.equal(terminal.provider, "removed_provider");
        assert.equal(terminal.provider_request_id, null);
        assert.equal(terminal.account_slot, null);
        assert.equal(terminal.account_cohort_id, null);
      } finally {
        console.info = originalInfo;
        setRemovedProviderTestAdapterForTest(null);
        setRemovedProviderApiKeyForTest(undefined);
        if (previousDebugRouting === undefined) kvStore.delete(debugKey);
        else kvStore.set(debugKey, previousDebugRouting);
        resetDebugRoutingCacheForTest();
      }
    });

    await t.step("Metered pre-header deadlines return an attributed 504 without retrying", async () => {
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
        let meteredAttempts = 0;
        await withFetchMock(
          (url) => {
            if (url === "https://api.openlux.ai/v1/responses") {
              meteredAttempts += 1;
              controller.abort(new DOMException("gateway deadline exceeded", "TimeoutError"));
              throw controller.signal.reason;
            }
            return authoritativeCodexQuotaResponse();
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
            assert.equal(response.headers.get("x-uos-upstream"), "metered", suffix);
            assert.equal(meteredAttempts, 1, suffix);
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

    await t.step("missing Metered bodies are recorded as ambiguous across routes and stream modes", async () => {
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
            if (url === "https://api.openlux.ai/v1/responses") {
              return new Response(null, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Api-Request-Id": `provider-${suffix}`,
                },
              });
            }
            return authoritativeCodexQuotaResponse();
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
              if (url === "https://api.openlux.ai/v1/responses") {
                return new Response(failureCase.body(), {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Api-Request-Id": `provider-${suffix}`,
                  },
                });
              }
              return authoritativeCodexQuotaResponse();
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
                assert.match(
                  responseText,
                  routeCase.route === "responses" ? /server_error/ : /upstream_stream_error/,
                  suffix,
                );
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
            if (url === "https://api.openlux.ai/v1/responses") {
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
            return authoritativeCodexQuotaResponse();
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

    await t.step("buffered post-header cancellation returns the OpenAI-shaped 499 contract", async () => {
      for (const route of ["responses", "chat"] as const) {
        const keyId = `fallback-buffered-cancel-${route}`;
        const requestId = `request-${keyId}`;
        seedPaidFallbackKey(keyId);
        const controller = new AbortController();
        const secondPull = new Deferred<void>();
        let emittedSemantic = false;
        let releaseBlockedPull = (): void => {};
        const response = await withFetchMock(
          (url) => {
            if (url === "https://api.openlux.ai/v1/responses") {
              return new Response(
                new ReadableStream<Uint8Array>({
                  pull(streamController) {
                    if (!emittedSemantic) {
                      emittedSemantic = true;
                      streamController.enqueue(
                        TEXT_ENCODER.encode(
                          'data: {"type":"response.output_text.delta","delta":"partial"}\n\n',
                        ),
                      );
                      return;
                    }
                    secondPull.resolve();
                    return new Promise<void>((resolve) => {
                      releaseBlockedPull = resolve;
                    });
                  },
                  cancel() {
                    releaseBlockedPull();
                  },
                }),
                {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Request-Id": `provider-buffered-cancel-${route}`,
                  },
                },
              );
            }
            return authoritativeCodexQuotaResponse();
          },
          async () => {
            const pending = route === "responses"
              ? handleResponses(
                new Request("https://ai.ubq.fi/v1/responses", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping" }),
                  signal: controller.signal,
                }),
                { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
              )
              : handleChatCompletions(
                new Request("https://ai.ubq.fi/v1/chat/completions", {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify({
                    model: DEFAULT_TEST_MODEL,
                    messages: [{ role: "user", content: "ping" }],
                  }),
                  signal: controller.signal,
                }),
                { keyId, kernelRepo: null, kernelOrg: null, requestId, startedAtMs: Date.now() },
              );
            await secondPull.promise;
            controller.abort(new DOMException("client disconnected", "AbortError"));
            return await pending;
          },
        );
        assert.equal(response.status, 499, route);
        assert.deepEqual(await response.json(), {
          error: {
            message: "Request was cancelled.",
            type: "server_error",
            code: "request_cancelled",
            param: null,
          },
        }, route);
        assert.equal(getResponseTelemetry(response)?.streamTerminalType, "cancelled", route);
        const stored = await waitForPaidFallbackTerminal(keyId, requestId, "cancelled");
        assert.equal(stored.dispatch_state, "dispatched", route);
      }
    });

    await t.step("validated terminals survive later client-body cancellation", async () => {
      for (const provider of ["chatgpt_codex", "metered"] as const) {
        for (const route of ["responses", "chat"] as const) {
          for (const terminalType of ["response.completed", "response.incomplete"] as const) {
            const suffix = `${provider}-${route}-${terminalType.replace(".", "-")}`;
            const keyId = `fallback-terminal-cancel-${suffix}`;
            const requestId = `request-${keyId}`;
            if (provider === "metered") seedPaidFallbackKey(keyId);
            const terminalState = terminalType === "response.completed" ? "completed" : "incomplete";
            const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];
            const context = {
              keyId: provider === "metered" ? keyId : null,
              kernelRepo: null,
              kernelOrg: null,
              requestId,
              startedAtMs: Date.now(),
              onTerminalUsage: (usage: { inputTokens: number | null } | null, completed: boolean) => {
                observedTerminalUsages.push({ completed, inputTokens: usage?.inputTokens ?? null });
              },
            };
            const atomicCommits: OpenAiAtomicOp[][] = [];
            const previousAtomicObserver = atomicCommitObservation.observer;
            if (provider === "metered") {
              resetProviderHealthThrottleForTest();
              atomicCommitObservation.observer = (operations) => atomicCommits.push([...operations]);
            }
            try {
              const response = await withFetchMock(
                (url) => {
                  if (provider === "metered" && url !== "https://api.openlux.ai/v1/responses") {
                    return authoritativeCodexQuotaResponse();
                  }
                  return new Response(
                    sseResponse([
                      `data: ${
                        JSON.stringify({
                          type: terminalType,
                          response: {
                            id: `resp_${suffix}`,
                            status: terminalState,
                            model: DEFAULT_TEST_MODEL,
                            output: terminalType === "response.completed"
                              ? [{
                                type: "message",
                                role: "assistant",
                                content: [{ type: "output_text", text: "terminal output" }],
                              }]
                              : [],
                            usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                          },
                        })
                      }\n\n`,
                    ]).body,
                    {
                      status: 200,
                      headers: {
                        "Content-Type": "text/event-stream",
                        "X-Request-Id": `provider-${suffix}`,
                      },
                    },
                  );
                },
                async () => {
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
                  assert.equal(response.status, 200, suffix);
                  assert.ok(response.body, suffix);
                  await response.body.cancel("client cancelled after upstream terminal");
                  return response;
                },
              );
              const telemetry = getResponseTelemetry(response);
              assert.equal(telemetry?.streamTerminalType, terminalType, suffix);
              assert.equal(telemetry?.completed, terminalType === "response.completed", suffix);
              assert.deepEqual(observedTerminalUsages, [{
                completed: terminalType === "response.completed",
                inputTokens: 1,
              }], suffix);
              if (provider === "metered") {
                const stored = await waitForPaidFallbackTerminal(keyId, requestId, terminalState);
                assert.equal(stored.dispatch_state, "dispatched", suffix);
                assert.notEqual(stored.terminal_state, "cancelled", suffix);
                assert.equal(stored.reconciliation_attempts, 1, suffix);

                const writesForKey = (key: Deno.KvKey): OpenAiAtomicOp[] =>
                  atomicCommits.flatMap((operations) =>
                    operations.filter((operation) =>
                      operation.type === "set" && keyToString(operation.key) === keyToString(key)
                    )
                  );
                const paidRequestKey = ["uos_ai", "paid_fallback", "v3", "request", keyId, requestId] as const;
                const terminalWrites = writesForKey(paidRequestKey).filter((operation) =>
                  typeof operation.value === "object" && operation.value !== null &&
                  (operation.value as { terminal_state?: unknown }).terminal_state === terminalState
                );
                assert.equal(terminalWrites.length, 1, `${suffix} terminal ledger transition`);
                assert.equal(
                  writesForKey(paidRequestKey).filter((operation) =>
                    typeof operation.value === "object" && operation.value !== null &&
                    (operation.value as { billing_state?: unknown }).billing_state === "settled"
                  ).length,
                  0,
                  `${suffix} has no unexpected settlement`,
                );

                const expectedHealthEvent = terminalType === "response.completed" ? "success" : "upstream_error";
                const expectedHealthStatus = terminalType === "response.completed" ? 200 : null;
                const healthKey = ["uos_ai", "provider_health", "v1", "metered", "default", "current"] as const;
                for (let attempt = 0; attempt < 100; attempt += 1) {
                  const healthWrites = writesForKey(healthKey).filter((operation) =>
                    typeof operation.value === "object" && operation.value !== null &&
                    (operation.value as { event?: unknown }).event === expectedHealthEvent
                  );
                  if (healthWrites.length === 1) break;
                  await new Promise<void>((resolve) => setTimeout(resolve, 1));
                }
                const terminalHealthWrites = writesForKey(healthKey).filter((operation) =>
                  typeof operation.value === "object" && operation.value !== null &&
                  (operation.value as { event?: unknown }).event === expectedHealthEvent
                );
                assert.equal(terminalHealthWrites.length, 1, `${suffix} terminal health transition`);
                const health = terminalHealthWrites[0]?.value as
                  | { status?: unknown; provider_request_id?: unknown }
                  | undefined;
                assert.equal(health?.status, expectedHealthStatus, suffix);
                assert.equal(health?.provider_request_id, `provider-${suffix}`, suffix);
              }
            } finally {
              atomicCommitObservation.observer = previousAtomicObserver;
              if (provider === "metered") resetProviderHealthThrottleForTest();
            }
          }
        }
      }
    });

    await t.step("validated Codex terminals resolve each half-open probe once", async () => {
      const authPoolKey = keyToString(["ubq_ai", "codex_auth"]);
      const routingKey = keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY);
      const previousAuthPool = kvStore.get(authPoolKey);
      const previousRouting = kvStore.get(routingKey);
      try {
        for (const route of ["responses", "chat"] as const) {
          for (const terminalType of ["response.completed", "response.incomplete"] as const) {
            const suffix = `${route}-${terminalType.replace(".", "-")}`;
            const accountId = `acct-terminal-probe-${suffix}`;
            const providerRequestId = `provider-terminal-probe-${suffix}`;
            const healthKey = ["uos_ai", "provider_health", "v1", "codex", accountId, "current"] as const;
            const terminalState = terminalType === "response.completed" ? "completed" : "incomplete";
            const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];
            const atomicCommits: OpenAiAtomicOp[][] = [];
            await withFetchMock(
              () =>
                new Response(
                  sseResponse([
                    `data: ${
                      JSON.stringify({
                        type: terminalType,
                        response: {
                          id: `resp_terminal_probe_${suffix}`,
                          status: terminalState,
                          model: DEFAULT_TEST_MODEL,
                          output: terminalType === "response.completed"
                            ? [{
                              type: "message",
                              role: "assistant",
                              content: [{ type: "output_text", text: "terminal output" }],
                            }]
                            : [],
                          usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                        },
                      })
                    }\n\n`,
                  ]).body,
                  {
                    status: 200,
                    headers: {
                      "Content-Type": "text/event-stream",
                      "X-Request-Id": providerRequestId,
                    },
                  },
                ),
              async () => {
                const existingPool = kvStore.get(authPoolKey) as CodexAuthPoolState;
                const account = existingPool.accounts[0]!;
                const pool = {
                  ...existingPool,
                  accounts: existingPool.accounts.map((entry, index) =>
                    index === 0 ? { ...entry, account_id: accountId } : entry
                  ),
                  updated_at_ms: Date.now(),
                };
                kvStore.set(authPoolKey, pool);
                resetCodexAuthCacheForTest();
                const credentialVersion = await sha256Hex(
                  `${accountId}\u0000${account.access_token}\u0000${account.refresh_token}`,
                );
                kvStore.set(routingKey, {
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
                resetCodexAccountRoutingForTest();
                resetProviderHealthThrottleForTest();
                const previousAtomicObserver = atomicCommitObservation.observer;
                atomicCommitObservation.observer = (operations) => atomicCommits.push([...operations]);
                try {
                  const context = {
                    keyId: null,
                    kernelRepo: null,
                    kernelOrg: null,
                    requestId: `request-terminal-probe-${suffix}`,
                    startedAtMs: Date.now(),
                    onTerminalUsage: (usage: { inputTokens: number | null } | null, completed: boolean) => {
                      observedTerminalUsages.push({ completed, inputTokens: usage?.inputTokens ?? null });
                    },
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
                  assert.equal(response.status, 200, suffix);
                  assert.ok(response.body, suffix);
                  await response.body.cancel("client cancelled after upstream terminal");

                  const writesForKey = (key: Deno.KvKey): OpenAiAtomicOp[] =>
                    atomicCommits.flatMap((operations) =>
                      operations.filter((operation) =>
                        operation.type === "set" && keyToString(operation.key) === keyToString(key)
                      )
                    );
                  const routingWrites = (): OpenAiAtomicOp[] => writesForKey(CODEX_ACCOUNT_ROUTING_KV_KEY);
                  const isProbeClaim = (operation: OpenAiAtomicOp): boolean => {
                    const slot = (operation.value as { slots?: Array<{ probe_lease?: unknown }> } | undefined)
                      ?.slots?.[0];
                    return slot?.probe_lease !== null && slot?.probe_lease !== undefined;
                  };
                  const isProbeClear = (operation: OpenAiAtomicOp): boolean => {
                    const slot = (operation.value as { slots?: Array<{ probe_lease?: unknown }> } | undefined)
                      ?.slots?.[0];
                    return slot?.probe_lease === null;
                  };
                  const expectedHealthEvent = terminalType === "response.completed" ? "success" : null;
                  for (let attempt = 0; attempt < 100; attempt += 1) {
                    const claims = routingWrites().filter(isProbeClaim);
                    const clears = routingWrites().filter(isProbeClear);
                    const healthWrites = writesForKey(healthKey).filter((operation) =>
                      typeof operation.value === "object" && operation.value !== null &&
                      (operation.value as { event?: unknown }).event === expectedHealthEvent
                    );
                    if (
                      claims.length === 1 && clears.length === 1 &&
                      (expectedHealthEvent === null || healthWrites.length === 1)
                    ) break;
                    await new Promise<void>((resolve) => setTimeout(resolve, 1));
                  }

                  const telemetry = getResponseTelemetry(response);
                  assert.equal(telemetry?.streamTerminalType, terminalType, suffix);
                  assert.equal(telemetry?.completed, terminalType === "response.completed", suffix);
                  assert.deepEqual(observedTerminalUsages, [{
                    completed: terminalType === "response.completed",
                    inputTokens: 1,
                  }], suffix);
                  assert.equal(routingWrites().filter(isProbeClaim).length, 1, `${suffix} probe claim`);
                  assert.equal(routingWrites().filter(isProbeClear).length, 1, `${suffix} probe clear`);

                  const terminalHealthWrites = writesForKey(healthKey).filter((operation) =>
                    typeof operation.value === "object" && operation.value !== null &&
                    ((operation.value as { event?: unknown }).event === "success" ||
                      (operation.value as { event?: unknown }).event === "upstream_error")
                  );
                  if (terminalType === "response.completed") {
                    assert.equal(terminalHealthWrites.length, 1, `${suffix} health transition`);
                    const health = terminalHealthWrites[0]?.value as
                      | { event?: unknown; status?: unknown; provider_request_id?: unknown }
                      | undefined;
                    assert.equal(health?.event, "success", suffix);
                    assert.equal(health?.status, 200, suffix);
                    assert.equal(health?.provider_request_id, providerRequestId, suffix);
                  } else {
                    assert.equal(terminalHealthWrites.length, 0, `${suffix} has no false health failure`);
                  }
                } finally {
                  atomicCommitObservation.observer = previousAtomicObserver;
                  resetProviderHealthThrottleForTest();
                }
              },
            );
          }
        }
      } finally {
        if (previousAuthPool === undefined) kvStore.delete(authPoolKey);
        else kvStore.set(authPoolKey, previousAuthPool);
        if (previousRouting === undefined) kvStore.delete(routingKey);
        else kvStore.set(routingKey, previousRouting);
        resetCodexAuthCacheForTest();
        resetCodexAccountRoutingForTest();
        resetProviderHealthThrottleForTest();
      }
    });

    await t.step("gateway logs a preflight terminal once after body cancellation", async () => {
      const originalInfo = console.info;
      const logs: unknown[][] = [];
      const providerRequestId = "provider-terminal-log-once";
      console.info = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          () =>
            new Response(
              sseResponse([
                `data: ${
                  JSON.stringify({
                    type: "response.completed",
                    response: {
                      id: "resp_terminal_log_once",
                      status: "completed",
                      model: DEFAULT_TEST_MODEL,
                      output: [{
                        id: "msg_terminal_log_once",
                        type: "message",
                        role: "assistant",
                        content: [{ type: "output_text", text: "done" }],
                      }],
                      usage: { input_tokens: 3, output_tokens: 2, total_tokens: 5 },
                    },
                  })
                }\n\n`,
              ]).body,
              {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Request-Id": providerRequestId,
                },
              },
            ),
          () =>
            gatewayHandler(
              new Request("http://localhost/v1/responses", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
              }),
            ),
        );
        assert.equal(response.status, 200);
        assert.ok(response.body);
        await response.body.cancel("client cancelled after upstream terminal");
        for (let attempt = 0; attempt < 100; attempt += 1) {
          const terminals = logs
            .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
            .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>)
            .filter((terminal) => terminal.provider_request_id === providerRequestId);
          if (terminals.length === 1) break;
          await new Promise<void>((resolve) => setTimeout(resolve, 1));
        }
        const terminals = logs
          .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
          .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>)
          .filter((terminal) => terminal.provider_request_id === providerRequestId);
        const cancelledTerminals = terminals.filter((terminal) => terminal.stream_terminal_type === "cancelled");
        assert.equal(terminals.length, 1);
        assert.equal(cancelledTerminals.length, 0);
        const terminal = terminals[0]!;
        assert.equal(terminal.status, 200);
        assert.equal(terminal.stream_terminal_type, "response.completed");
        assert.equal(terminal.input_tokens, 3);
        assert.equal(terminal.output_tokens, 2);
        assert.equal(terminal.total_tokens, 5);
      } finally {
        console.info = originalInfo;
      }
    });

    await t.step("buffered Chat preflight terminals record usage exactly once", async () => {
      for (const terminalType of ["response.completed", "response.incomplete"] as const) {
        const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];
        const response = await withFetchMock(
          () =>
            new Response(
              sseResponse([
                `data: ${
                  JSON.stringify({
                    type: terminalType,
                    response: {
                      id: `resp-buffered-chat-${terminalType}`,
                      status: terminalType === "response.completed" ? "completed" : "incomplete",
                      model: DEFAULT_TEST_MODEL,
                      output: terminalType === "response.completed"
                        ? [{
                          type: "message",
                          role: "assistant",
                          content: [{ type: "output_text", text: "terminal output" }],
                        }]
                        : [],
                      usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
                    },
                  })
                }\n\n`,
              ]).body,
              {
                status: 200,
                headers: { "Content-Type": "text/event-stream" },
              },
            ),
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
                keyId: null,
                kernelRepo: null,
                kernelOrg: null,
                onTerminalUsage: (usage, completed) => {
                  observedTerminalUsages.push({ completed, inputTokens: usage?.inputTokens ?? null });
                },
              },
            ),
        );
        assert.equal(response.status, terminalType === "response.completed" ? 200 : 502, terminalType);
        if (response.body) await response.body.cancel("client cancelled after buffered upstream terminal");
        assert.deepEqual(observedTerminalUsages, [{
          completed: terminalType === "response.completed",
          inputTokens: 1,
        }], terminalType);
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
          if (url === "https://api.openlux.ai/v1/responses") {
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
          return authoritativeCodexQuotaResponse();
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

    await t.step("Metered HTTP errors use OpenAI envelopes without changing routing", async (t) => {
      const cases = [
        {
          name: "responses preserves an existing error envelope and 429",
          route: "responses",
          status: 429,
          statusText: "Metered Rate Limited",
          body: JSON.stringify({
            error: {
              message: "Metered is rate limited.",
              type: "rate_limit_error",
              code: "provider_rate_limit",
              param: null,
            },
            opaque: { drop: true },
          }),
          retryAfter: "17",
          expectedError: {
            message: "Metered is rate limited.",
            type: "rate_limit_error",
            code: "provider_rate_limit",
            param: null,
          },
        },
        {
          name: "chat completions parses a provider-root message and preserves 502",
          route: "chat.completions",
          status: 502,
          statusText: "Metered Bad Gateway",
          body: JSON.stringify({
            message: "Metered could not reach its model backend.",
            type: "server_error",
            code: "provider_unavailable",
            opaque: { drop: true },
          }),
          retryAfter: null,
          expectedError: {
            message: "Metered could not reach its model backend.",
            type: "server_error",
            code: "provider_unavailable",
          },
        },
        {
          name: "chat completions converts plain text and preserves 401",
          route: "chat.completions",
          status: 401,
          statusText: "Metered Unauthorized",
          body: "Metered rejected the configured credential.",
          retryAfter: null,
          expectedError: {
            message: "Metered rejected the configured credential.",
            type: "invalid_request_error",
            code: "upstream_error",
          },
        },
        {
          name: "responses classifies an untyped upstream 429 as rate limited",
          route: "responses",
          status: 429,
          statusText: "Metered Rate Limited",
          body: JSON.stringify({ detail: "Metered has no capacity." }),
          retryAfter: "3",
          expectedError: {
            message: "Metered has no capacity.",
            type: "rate_limit_error",
            code: "upstream_error",
          },
        },
      ] as const;

      for (const [index, testCase] of cases.entries()) {
        await t.step(testCase.name, async () => {
          const keyId = `fallback-metered-normalized-${index}`;
          const requestId = `request-fallback-metered-normalized-${index}`;
          seedPaidFallbackKey(keyId);
          let codexCalls = 0;
          let meteredCalls = 0;
          const response = await withFetchMock(
            (url) => {
              if (url === "https://api.openlux.ai/v1/responses") {
                meteredCalls += 1;
                const headers = new Headers({
                  "Content-Type": "application/problem+json",
                  "X-Metered-Diagnostic": "drop-me",
                });
                if (testCase.retryAfter) headers.set("Retry-After", testCase.retryAfter);
                return new Response(testCase.body, {
                  status: testCase.status,
                  statusText: testCase.statusText,
                  headers,
                });
              }
              codexCalls += 1;
              return authoritativeCodexQuotaResponse();
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
          assert.equal(response.headers.get("x-uos-upstream"), "metered");
          assert.equal(response.headers.get("Retry-After"), testCase.retryAfter);
          assert.equal(response.headers.get("X-Metered-Diagnostic"), null);
          assert.deepEqual(await response.json(), { error: testCase.expectedError });
          assert.equal(codexCalls, 1);
          assert.equal(meteredCalls, 1);
          const failed = await waitForPaidFallbackTerminal(keyId, requestId, "failed");
          assert.equal(failed.terminal_state, "failed");
        });
      }
    });

    await t.step("a ledger write failure after Metered accepts preserves the usable response", async () => {
      const keyId = "fallback-ledger-write-failure";
      const requestId = "request-fallback-ledger-write-failure";
      const providerRequestId = "metered-ledger-write-failure";
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
            if (url === "https://api.openlux.ai/v1/responses") {
              return new Response(sseResponse(baseSseChunks()).body, {
                status: 200,
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            return authoritativeCodexQuotaResponse();
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
          const providerRequestId = `metered-reconcile-${suffix}`;
          seedPaidFallbackKey(keyId);
          const result = await withFetchMock(
            (url) => {
              if (url === "https://api.openlux.ai/v1/responses") {
                return new Response(sseResponse(baseSseChunks()).body, {
                  status: 200,
                  headers: {
                    "Content-Type": "text/event-stream",
                    "X-Oneapi-Request-Id": providerRequestId,
                  },
                });
              }
              if (url === "https://api.openlux.ai/api/log/token") {
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
              return authoritativeCodexQuotaResponse();
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

    await t.step("reconciliation failure does not replace the original Metered error", async () => {
      const keyId = "fallback-error-reconcile-failure";
      const requestId = "request-fallback-error-reconcile-failure";
      const providerRequestId = "metered-error-reconcile-failure";
      seedPaidFallbackKey(keyId);
      exposePaidFallbackLedgerEntries = true;
      atomicCommitFailure = (ops) =>
        ops.some((op) => (op.value as { billing_status?: unknown } | undefined)?.billing_status === "reconciled")
          ? new Error("injected upstream error reconciliation failure")
          : null;
      try {
        const response = await withFetchMock(
          (url) => {
            if (url === "https://api.openlux.ai/v1/responses") {
              return new Response(JSON.stringify({ error: { message: "Metered original error" } }), {
                status: 503,
                headers: {
                  "Content-Type": "application/json",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            }
            if (url === "https://api.openlux.ai/api/log/token") {
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
            return authoritativeCodexQuotaResponse();
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
        assert.equal(payload.error?.message, "Metered original error");
      } finally {
        atomicCommitFailure = null;
        exposePaidFallbackLedgerEntries = false;
      }
    });
  } finally {
    atomicCommitFailure = null;
    exposePaidFallbackLedgerEntries = false;
    kvStore.delete(keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY));
    resetCodexAccountRoutingForTest();
    resetCodexAuthCacheForTest();
    if (originalApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalApiKey);
  }
});

Deno.test("http: CORS wrapper exposes a gateway request id", () => {
  const response = withCors(new Response("{}", { headers: { "Content-Type": "application/json" } }));
  assert.ok(response.headers.get("x-uos-request-id"));
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-request-id/);
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-provider-request-id/);
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-uos-upstream/);
  assert.match(response.headers.get("Access-Control-Expose-Headers") ?? "", /x-ratelimit-remaining-tokens-minute/);
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

  await t.step("responses preserves official direct tools and tool_choice", async () => {
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
                  name: "fetch_weather",
                  description: "Fetch weather for a city.",
                  parameters: { type: "object", properties: { city: { type: "string" } } },
                  strict: true,
                },
              ],
              tool_choice: { type: "function", name: "fetch_weather" },
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
    assert.equal(recordedTools[0]?.strict, true);
    assert.equal(Object.prototype.hasOwnProperty.call(recordedTools[0], "function"), false);
    const recordedToolChoice = recorded["tool_choice"] as Record<string, unknown> | undefined;
    assert.ok(recordedToolChoice);
    assert.equal(recordedToolChoice.type, "function");
    assert.equal(recordedToolChoice.name, "fetch_weather");
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "function"), false);
  });

  await t.step("responses flattens nested compatibility tools and tool_choice", async () => {
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
              tool_choice: { type: "function", function: { name: "fetch_weather" } },
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
    const recordedToolChoice = recorded["tool_choice"] as Record<string, unknown> | undefined;
    assert.ok(recordedToolChoice);
    assert.equal(recordedToolChoice.type, "function");
    assert.equal(recordedToolChoice.name, "fetch_weather");
    assert.equal(Object.prototype.hasOwnProperty.call(recordedToolChoice, "function"), false);
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

Deno.test("openai: Chat assistant top-level refusal replays as output text", async () => {
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
            messages: [{ role: "assistant", content: null, refusal: "Cannot help." }],
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

Deno.test("openai: malformed preflight terminal emits a Chat stream error", async () => {
  const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              output: [{ id: "fc_preflight_bad", type: "function_call", call_id: "call_bad", name: "bad" }],
              usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
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
        {
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          onTerminalUsage: (usage, completed) => {
            observedTerminalUsages.push({ completed, inputTokens: usage?.inputTokens ?? null });
          },
        },
      ),
  );

  const text = await response.text();
  assert.match(text, /upstream_stream_error/);
  assert.doesNotMatch(text, /data: \[DONE\]/);
  assert.deepEqual(observedTerminalUsages, [{ completed: false, inputTokens: 1 }]);
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

Deno.test("openai: contentless response.completed fails Chat without success framing", async (t) => {
  const usage = { input_tokens: 1642, output_tokens: 2048, total_tokens: 3690 };
  const completed = `data: ${
    JSON.stringify({
      type: "response.completed",
      response: { id: "resp_empty", model: DEFAULT_TEST_MODEL, output: [], usage },
    })
  }\n\n`;

  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const observedTerminalUsages: Array<{ completed: boolean; inputTokens: number | null }> = [];
      const response = await withFetchMock(
        () => sseResponse([completed]),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                reasoning_effort: "low",
                max_completion_tokens: 2048,
                messages: [{ role: "user", content: "contentless completion" }],
              }),
            }),
            {
              keyId: null,
              kernelRepo: null,
              kernelOrg: null,
              onTerminalUsage: (terminalUsage, terminalCompleted) => {
                observedTerminalUsages.push({
                  completed: terminalCompleted,
                  inputTokens: terminalUsage?.inputTokens ?? null,
                });
              },
            },
          ),
      );

      assert.equal(response.status, 502);
      assert.doesNotMatch(response.headers.get("Content-Type") ?? "", /text\/event-stream/i);
      assert.deepEqual(await response.json(), {
        error: {
          message: "Upstream response completed with no translated semantic output.",
          type: "server_error",
          code: "empty_upstream_completion",
          param: null,
        },
      });

      const telemetry = getResponseTelemetry(response);
      assert.equal(telemetry?.completed, false);
      assert.equal(telemetry?.semanticOutputObserved, false);
      assert.equal(telemetry?.outputTokenAllowance, 2048);
      assert.deepEqual(telemetry?.upstreamEventKinds, ["response.completed"]);
      assert.equal(telemetry?.streamTerminalType, "error");
      assert.equal(telemetry?.failureKind, "empty_upstream_completion");
      assert.equal(telemetry?.inputTokens, 1642);
      assert.equal(telemetry?.outputTokens, 2048);
      assert.equal(telemetry?.totalTokens, 3690);
      assert.deepEqual(observedTerminalUsages, [{ completed: false, inputTokens: 1642 }]);
    });
  }
});

Deno.test("openai: partial Chat output followed by failure is not an empty completion", async (t) => {
  const events = [
    `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "partial output" })}\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.failed",
        response: {
          status: "failed",
          output: [],
          usage: { input_tokens: 2, output_tokens: 3, total_tokens: 5 },
        },
      })
    }\n\n`,
  ];
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(events),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "partial then fail" }],
              }),
            }),
          ),
      );
      const serialized = await response.text();
      assert.match(serialized, /upstream_stream_error/);
      assert.doesNotMatch(serialized, /empty_upstream_completion|data: \[DONE\]/);
      assert.equal(getResponseTelemetry(response)?.semanticOutputObserved, true);
      assert.equal(getResponseTelemetry(response)?.completed, false);
    });
  }
});

Deno.test("openai: empty Chat terminal diagnostics are bounded and content-free", async () => {
  const secretPrompt = "private prompt that must not enter diagnostics";
  const secretEventPayload = "private provider output that must not enter diagnostics";
  const unknownEventType = "response.future_private_output";
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await withFetchMock(
      () =>
        new Response(
          sseResponse([
            `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_empty_log" } })}\n\n`,
            `data: ${JSON.stringify({ type: "response.in_progress", response: { id: "resp_empty_log" } })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.reasoning_summary_text.delta",
                summary_index: 0,
                delta: "private reasoning summary",
              })
            }\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.reasoning_summary_text.done",
                summary_index: 0,
                text: "private reasoning summary",
              })
            }\n\n`,
            `data: ${
              JSON.stringify({ type: "response.reasoning_text.delta", content_index: 0, delta: "private reasoning" })
            }\n\n`,
            `data: ${
              JSON.stringify({ type: "response.reasoning_text.done", content_index: 0, text: "private reasoning" })
            }\n\n`,
            `data: ${JSON.stringify({ type: "response.reasoning_summary_part.added", summary_index: 0 })}\n\n`,
            `data: ${JSON.stringify({ type: "response.reasoning_summary_part.done", summary_index: 0 })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.content_part.added",
                item_id: "msg_empty_log",
                content_index: 0,
                part: { type: "output_text", text: "" },
              })
            }\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.content_part.done",
                item_id: "msg_empty_log",
                content_index: 0,
                part: { type: "output_text", text: "" },
              })
            }\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.custom_tool_call_input.delta",
                item_id: "tool_empty_log",
                delta: "private tool input",
              })
            }\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.custom_tool_call_input.done",
                item_id: "tool_empty_log",
                input: "private tool input",
              })
            }\n\n`,
            `data: ${JSON.stringify({ type: unknownEventType, output: secretEventPayload })}\n\n`,
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: {
                  id: "resp_empty_log",
                  model: DEFAULT_TEST_MODEL,
                  output: [],
                  usage: { input_tokens: 10, output_tokens: 2048, total_tokens: 2058 },
                },
              })
            }\n\n`,
          ]).body,
          {
            status: 200,
            headers: {
              "Content-Type": "text/event-stream",
              "X-Request-Id": "provider-empty-log",
            },
          },
        ),
      () =>
        handleChatCompletions(
          new Request("https://ai.ubq.fi/v1/chat/completions", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              model: DEFAULT_TEST_MODEL,
              reasoning_effort: "low",
              max_completion_tokens: 2048,
              messages: [{ role: "user", content: secretPrompt }],
            }),
          }),
        ),
    );
    const loggedResponse = await withTerminalRequestLog(response, {
      route: "chat.completions",
      telemetryResponse: response,
      startedAtMonotonicMs: performance.now(),
      requestId: "empty-chat-terminal-log",
    });
    assert.equal(loggedResponse.status, 502);
    await loggedResponse.json();

    for (let attempt = 0; attempt < 100; attempt += 1) {
      if (logs.some((entry) => entry[0] === "[ai.ubq.fi] request_terminal")) break;
      await new Promise<void>((resolve) => setTimeout(resolve, 1));
    }
    const terminalLogs = logs
      .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
      .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>);
    assert.equal(terminalLogs.length, 1);
    const terminal = terminalLogs[0]!;
    assert.equal(terminal.provider_request_id, "provider-empty-log");
    assert.equal(terminal.output_token_allowance, 2048);
    assert.equal(terminal.semantic_output_observed, false);
    assert.deepEqual(terminal.upstream_event_kinds, [
      "response.created",
      "response.in_progress",
      "response.reasoning_summary_text.delta",
      "response.reasoning_summary_text.done",
      "response.reasoning_text.delta",
      "response.reasoning_text.done",
      "response.reasoning_summary_part.added",
      "response.reasoning_summary_part.done",
      "response.content_part.added",
      "response.content_part.done",
      "response.custom_tool_call_input.delta",
      "response.custom_tool_call_input.done",
      "unrecognized",
      "response.completed",
    ]);
    assert.equal(terminal.stream_terminal_type, "error");
    assert.equal(terminal.failure_kind, "empty_upstream_completion");
    assert.equal(terminal.input_tokens, 10);
    assert.equal(terminal.output_tokens, 2048);
    const serializedTerminal = JSON.stringify(terminal);
    assert.equal(serializedTerminal.includes(secretPrompt), false);
    assert.equal(serializedTerminal.includes(secretEventPayload), false);
    assert.equal(serializedTerminal.includes(unknownEventType), false);
  } finally {
    console.info = originalInfo;
  }
});

Deno.test("openai: Chat refusal output remains semantic in buffered and streamed modes", async (t) => {
  const refusalText = "I cannot help with that request.";
  const refusalItem = {
    id: "msg_refusal",
    type: "message",
    role: "assistant",
    content: [{ type: "refusal", refusal: refusalText }],
  };
  const cases = [
    {
      name: "refusal delta and done",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.refusal.delta",
            item_id: "msg_refusal",
            output_index: 0,
            content_index: 0,
            delta: "I cannot ",
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.refusal.done",
            item_id: "msg_refusal",
            output_index: 0,
            content_index: 0,
            refusal: refusalText,
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "final response output refusal",
      events: [
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [refusalItem] } })}\n\n`,
      ],
    },
    {
      name: "output-item-done refusal",
      events: [
        `data: ${JSON.stringify({ type: "response.output_item.done", output_index: 0, item: refusalItem })}\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "content-part-done refusal",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.content_part.done",
            item_id: "msg_refusal",
            output_index: 0,
            content_index: 0,
            part: { type: "refusal", refusal: refusalText },
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "response.output plus repeated output-item-done refusal",
      events: [
        `data: ${JSON.stringify({ type: "response.output", output: [refusalItem] })}\n\n`,
        `data: ${JSON.stringify({ type: "response.output_item.done", output_index: 0, item: refusalItem })}\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "response.output plus repeated content-part-done refusal",
      events: [
        `data: ${JSON.stringify({ type: "response.output", output: [refusalItem] })}\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.content_part.done",
            item_id: "msg_refusal",
            output_index: 0,
            content_index: 0,
            part: refusalItem.content[0],
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
  ];

  for (const testCase of cases) {
    for (const stream of [false, true]) {
      await t.step(`${testCase.name} (${stream ? "streamed" : "buffered"})`, async () => {
        const response = await withFetchMock(
          () => sseResponse(testCase.events),
          () =>
            handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  stream,
                  messages: [{ role: "user", content: "refuse" }],
                }),
              }),
            ),
        );
        assert.equal(response.status, 200);
        if (!stream) {
          const payload = await response.json() as {
            choices?: Array<{ message?: { content?: unknown; refusal?: unknown }; finish_reason?: unknown }>;
          };
          assert.equal(payload.choices?.[0]?.message?.content, null);
          assert.equal(payload.choices?.[0]?.message?.refusal, refusalText);
          assert.equal(payload.choices?.[0]?.finish_reason, "stop");
        } else {
          const serialized = await response.text();
          const chunks = [...serialized.matchAll(/^data: (.+)$/gm)]
            .map((match) => match[1]!)
            .filter((value) => value !== "[DONE]")
            .map((value) =>
              JSON.parse(value) as {
                choices?: Array<{ delta?: { refusal?: unknown }; finish_reason?: unknown }>;
              }
            );
          const refusal = chunks.map((chunk) => chunk.choices?.[0]?.delta?.refusal)
            .filter((value): value is string => typeof value === "string")
            .join("");
          assert.equal(refusal, refusalText);
          assert.equal(chunks.filter((chunk) => chunk.choices?.[0]?.finish_reason === "stop").length, 1);
          assert.equal(serialized.match(/data: \[DONE\]/g)?.length, 1);
        }
        assert.equal(getResponseTelemetry(response)?.semanticOutputObserved, true);
        assert.equal(getResponseTelemetry(response)?.completed, true);
      });
    }
  }
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

Deno.test("openai: Chat recovers completed output text without duplicating streamed deltas", async (t) => {
  const completedText = '{"subjects":[{"title":"Recovered"}]}';
  const finalOutput = [{
    id: "msg_done_text",
    type: "message",
    role: "assistant",
    content: [{ type: "output_text", text: completedText }],
  }];
  const cases = [
    {
      name: "done-only text with empty terminal output",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.output_text.done",
            item_id: "msg_done_text",
            output_index: 0,
            content_index: 0,
            text: completedText,
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "output-item-done text with empty terminal output",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.output_item.done",
            output_index: 0,
            item: finalOutput[0],
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "content-part-done text with empty terminal output",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.content_part.done",
            item_id: "msg_done_text",
            output_index: 0,
            content_index: 0,
            part: { type: "output_text", text: completedText, annotations: [] },
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "delta prefix plus repeated done and terminal text",
      events: [
        `data: ${
          JSON.stringify({
            type: "response.output_text.delta",
            item_id: "msg_done_text",
            output_index: 0,
            content_index: 0,
            delta: completedText.slice(0, 12),
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.output_text.done",
            item_id: "msg_done_text",
            output_index: 0,
            content_index: 0,
            text: completedText,
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: finalOutput } })}\n\n`,
      ],
    },
    {
      name: "response.output plus repeated output-item-done text",
      events: [
        `data: ${JSON.stringify({ type: "response.output", output: finalOutput })}\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.output_item.done",
            output_index: 0,
            item: finalOutput[0],
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
    {
      name: "response.output plus repeated content-part-done text",
      events: [
        `data: ${JSON.stringify({ type: "response.output", output: finalOutput })}\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.content_part.done",
            item_id: "msg_done_text",
            output_index: 0,
            content_index: 0,
            part: finalOutput[0].content[0],
          })
        }\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ],
    },
  ];

  for (const testCase of cases) {
    for (const stream of [false, true]) {
      await t.step(`${testCase.name} (${stream ? "streamed" : "buffered"})`, async () => {
        const response = await withFetchMock(
          () => sseResponse(testCase.events),
          () =>
            handleChatCompletions(
              new Request("https://ai.ubq.fi/v1/chat/completions", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                  model: DEFAULT_TEST_MODEL,
                  stream,
                  messages: [{ role: "user", content: "subjects" }],
                }),
              }),
            ),
        );
        assert.equal(response.status, 200);
        if (!stream) {
          const payload = await response.json() as { choices?: Array<{ message?: { content?: unknown } }> };
          assert.equal(payload.choices?.[0]?.message?.content, completedText);
          return;
        }

        const serialized = await response.text();
        const chunks = [...serialized.matchAll(/^data: (.+)$/gm)]
          .map((match) => match[1]!)
          .filter((value) => value !== "[DONE]")
          .map((value) =>
            JSON.parse(value) as {
              choices?: Array<{ delta?: { content?: unknown }; finish_reason?: unknown }>;
            }
          );
        const content = chunks.map((chunk) => chunk.choices?.[0]?.delta?.content)
          .filter((value): value is string => typeof value === "string")
          .join("");
        assert.equal(content, completedText);
        assert.equal(chunks.filter((chunk) => chunk.choices?.[0]?.finish_reason === "stop").length, 1);
        assert.equal(serialized.match(/data: \[DONE\]/g)?.length, 1);
      });
    }
  }
});

Deno.test("openai: contentless native Responses and reasoning-only completions fail before success", async (t) => {
  const cases = [
    { variant: "empty", surfaces: ["responses"] as const },
    { variant: "reasoning_only", surfaces: ["chat", "responses"] as const },
  ] as const;

  for (const testCase of cases) {
    for (const surface of testCase.surfaces) {
      for (const stream of [false, true]) {
        await t.step(`${testCase.variant} ${surface} ${stream ? "streamed" : "buffered"}`, async () => {
          const observations: boolean[] = [];
          const observedInputTokens: Array<number | null> = [];
          let fetches = 0;
          const response = await withFetchMock(
            () => {
              fetches += 1;
              return sseResponse([
                `data: ${
                  JSON.stringify({ type: "response.created", response: { id: `resp_${testCase.variant}` } })
                }\n\n`,
                ...(testCase.variant === "reasoning_only"
                  ? [
                    `data: ${
                      JSON.stringify({
                        type: "response.reasoning_summary_text.delta",
                        response_id: `resp_${testCase.variant}`,
                        item_id: `reasoning_${testCase.variant}`,
                        output_index: 0,
                        summary_index: 0,
                        delta: "hidden reasoning",
                      })
                    }\n\n`,
                  ]
                  : []),
                `data: ${
                  JSON.stringify({
                    type: "response.completed",
                    response: {
                      id: `resp_${testCase.variant}`,
                      status: "completed",
                      output: testCase.variant === "reasoning_only"
                        ? [{ type: "reasoning", summary: [{ type: "summary_text", text: "hidden reasoning" }] }]
                        : [],
                      usage: { input_tokens: 3, output_tokens: 4, total_tokens: 7 },
                    },
                  })
                }\n\n`,
              ]);
            },
            () =>
              surface === "chat"
                ? handleChatCompletions(
                  new Request("https://ai.ubq.fi/v1/chat/completions", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({
                      model: DEFAULT_TEST_MODEL,
                      stream,
                      messages: [{ role: "user", content: "hello" }],
                    }),
                  }),
                  {
                    keyId: null,
                    kernelRepo: null,
                    kernelOrg: null,
                    onTerminalUsage: (usage, completed) => {
                      observations.push(completed);
                      observedInputTokens.push(usage?.inputTokens ?? null);
                    },
                  },
                )
                : handleResponses(responsesRequest({ stream }), {
                  keyId: null,
                  kernelRepo: null,
                  kernelOrg: null,
                  onTerminalUsage: (usage, completed) => {
                    observations.push(completed);
                    observedInputTokens.push(usage?.inputTokens ?? null);
                  },
                }),
          );
          const releasedReasoningStream = testCase.variant === "reasoning_only" && stream;
          if (releasedReasoningStream) {
            assert.equal(response.status, 200);
            assert.match(response.headers.get("Content-Type") ?? "", /text\/event-stream/i);
            const serialized = await response.text();
            assert.match(serialized, /empty_upstream_completion/);
            assert.doesNotMatch(serialized, /data: \[DONE\]/);
          } else {
            assert.equal(response.status, 502);
            assert.doesNotMatch(response.headers.get("Content-Type") ?? "", /text\/event-stream/i);
            const payload = await response.json() as { error?: { code?: string; type?: string; param?: unknown } };
            assert.equal(payload.error?.code, "empty_upstream_completion");
            assert.equal(payload.error?.type, "server_error");
            assert.equal(payload.error?.param, null);
          }
          const telemetry = getResponseTelemetry(response);
          assert.equal(telemetry?.completed, false);
          assert.equal(telemetry?.failureKind, "empty_upstream_completion");
          assert.equal(telemetry?.semanticOutputObserved, false);
          assert.equal(telemetry?.inputTokens, 3);
          assert.equal(telemetry?.outputTokens, 4);
          assert.equal(telemetry?.totalTokens, 7);
          assert.deepEqual(observations, [false]);
          assert.deepEqual(observedInputTokens, [3]);
          assert.equal(fetches, 1);
        });
      }
    }
  }
});

Deno.test("openai: native Responses preserves a refusal-only terminal", async (t) => {
  const refusalItem = {
    id: "msg_native_refusal",
    type: "message",
    role: "assistant",
    status: "completed",
    content: [{ type: "refusal", refusal: "Request declined." }],
  };
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () =>
          sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.completed",
                response: { id: "resp_native_refusal", status: "completed", output: [refusalItem] },
              })
            }\n\n`,
          ]),
        () => handleResponses(responsesRequest({ stream })),
      );
      assert.equal(response.status, 200);
      if (stream) {
        assert.match(await response.text(), /Request declined\./);
      } else {
        const payload = await response.json() as { output?: unknown };
        assert.deepEqual(payload.output, [refusalItem]);
      }
    });
  }
});

Deno.test("openai: Chat concatenates multiple finalized message items", async (t) => {
  const items = [
    {
      id: "msg_first",
      type: "message",
      role: "assistant",
      content: [{ type: "output_text", text: "First. " }],
    },
    {
      id: "msg_second",
      type: "message",
      role: "assistant",
      content: [{ type: "output_text", text: "Second." }],
    },
  ];
  const events = [
    ...items.map((item, outputIndex) =>
      `data: ${JSON.stringify({ type: "response.output_item.done", output_index: outputIndex, item })}\n\n`
    ),
    `data: ${JSON.stringify({ type: "response.completed", response: { output: items } })}\n\n`,
  ];

  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        () => sseResponse(events),
        () =>
          handleChatCompletions(
            new Request("https://ai.ubq.fi/v1/chat/completions", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                model: DEFAULT_TEST_MODEL,
                stream,
                messages: [{ role: "user", content: "two messages" }],
              }),
            }),
          ),
      );
      assert.equal(response.status, 200);
      if (!stream) {
        const payload = await response.json() as { choices?: Array<{ message?: { content?: unknown } }> };
        assert.equal(payload.choices?.[0]?.message?.content, "First. Second.");
      } else {
        const serialized = await response.text();
        const chunks = [...serialized.matchAll(/^data: (.+)$/gm)]
          .map((match) => match[1]!)
          .filter((value) => value !== "[DONE]")
          .map((value) =>
            JSON.parse(value) as {
              choices?: Array<{ delta?: { content?: unknown }; finish_reason?: unknown }>;
            }
          );
        const content = chunks.map((chunk) => chunk.choices?.[0]?.delta?.content)
          .filter((value): value is string => typeof value === "string")
          .join("");
        assert.equal(content, "First. Second.");
        assert.equal(chunks.filter((chunk) => chunk.choices?.[0]?.finish_reason === "stop").length, 1);
        assert.equal(serialized.match(/data: \[DONE\]/g)?.length, 1);
      }
      assert.equal(getResponseTelemetry(response)?.semanticOutputObserved, true);
      assert.equal(getResponseTelemetry(response)?.completed, true);
    });
  }
});

Deno.test("openai: native Responses preserve files and key while omitting unsupported cache controls", async () => {
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
  const warnings = parseWarnings(response.headers.get("x-uos-warning"));
  assert.ok(warnings.includes("prompt_cache_options_ignored"));
  assert.ok(warnings.includes("prompt_cache_retention_ignored"));
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal("instructions" in recorded, false);
  assert.equal(recorded.prompt_cache_key, "cache-key");
  assert.equal("prompt_cache_options" in recorded, false);
  assert.equal("prompt_cache_retention" in recorded, false);
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
        JSON.stringify({
          type: "response.completed",
          response: {
            model: DEFAULT_TEST_MODEL,
            output: [{
              type: "message",
              role: "assistant",
              content: [{ type: "output_text", text: "cache telemetry output" }],
            }],
            usage,
          },
        })
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

  await t.step("keyed Codex warnings retain cache usage through persisted analytics", async () => {
    const analyticsKv = new CountingKv();
    const analyticsNow = 1_800_000_000_000;
    const authKey = keyToString(["ubq_ai", "codex_auth"]);
    const routingKey = keyToString(CODEX_ACCOUNT_ROUTING_KV_KEY);
    const previousAuth = kvStore.get(authKey);
    const previousRouting = kvStore.get(routingKey);
    const affinityKeys: string[] = [];
    const now = Date.now();
    const accessToken = (label: string): string =>
      `${encodeJsonBase64Url({ alg: "none" })}.${
        encodeJsonBase64Url({ exp: Math.floor((now + 60 * 60_000) / 1_000) })
      }.${label}`;
    const accountOne = {
      access_token: accessToken("affinity-account-one"),
      refresh_token: "affinity-refresh-one",
      account_id: "affinity-account-one",
      updated_at_ms: now,
    };
    const accountTwo = {
      access_token: accessToken("affinity-account-two"),
      refresh_token: "affinity-refresh-two",
      account_id: "affinity-account-two",
      updated_at_ms: now,
    };
    const preferredAccountHash = await sha256Hex(
      `uos_ai\u0000codex_routing_account\u0000${accountOne.account_id}`,
    );
    const analyticsUsage = {
      input_tokens: 2048,
      input_tokens_details: { cached_tokens: 1024, cache_write_tokens: 512 },
      output_tokens: 16,
      total_tokens: 2064,
    };
    const analyticsCompleted = () =>
      sseResponse([
        `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_cache_analytics" } })}\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: {
              model: DEFAULT_TEST_MODEL,
              output: [{
                type: "message",
                role: "assistant",
                content: [{ type: "output_text", text: "cache analytics output" }],
              }],
              usage: analyticsUsage,
            },
          })
        }\n\n`,
      ]);
    const requests = [
      {
        route: "chat.completions",
        request: new Request("https://ai.ubq.fi/v1/chat/completions", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            prompt_cache_key: "stable-analytics-key",
            prompt_cache_options: { ttl: "30m" },
            messages: [{ role: "user", content: "stable cache analytics prefix" }],
          }),
        }),
        handle: handleChatCompletions,
        expectsCacheOptionsWarning: true,
        expectedAffinityOutcome: "preferred",
        expectedAccountId: accountOne.account_id,
        expectedPromptCacheMode: "implicit",
        accounts: [accountTwo, accountOne],
      },
      {
        route: "responses",
        request: new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            model: DEFAULT_TEST_MODEL,
            prompt_cache_key: "stable-analytics-key",
            input: "stable cache analytics prefix",
          }),
        }),
        handle: handleResponses,
        expectsCacheOptionsWarning: false,
        expectedAffinityOutcome: "remapped",
        expectedAccountId: accountTwo.account_id,
        expectedPromptCacheMode: "unspecified",
        accounts: [accountTwo],
      },
    ] as const;

    try {
      for (const [index, fixture] of requests.entries()) {
        const keyId = `affinity-analytics-${index}`;
        const principal = `api-key:${keyId}`;
        const identity = await deriveCodexAccountAffinityIdentity(principal, "stable-analytics-key");
        assert.ok(identity);
        affinityKeys.push(keyToString(identity.kvKey));
        kvStore.set(authKey, { accounts: fixture.accounts, updated_at_ms: now } satisfies CodexAuthPoolState);
        kvStore.delete(routingKey);
        resetCodexAuthCacheForTest();
        resetCodexAccountRoutingForTest();
        await recordCodexAccountAffinity(identity, preferredAccountHash, now);

        const forwarded: { body: Record<string, unknown> | null; accountId: string | null } = {
          body: null,
          accountId: null,
        };
        const response = await withFetchMock(
          (_url, bodyText, init) => {
            forwarded.body = JSON.parse(bodyText ?? "{}") as Record<string, unknown>;
            forwarded.accountId = new Headers(init?.headers).get("ChatGPT-Account-ID");
            return analyticsCompleted();
          },
          () =>
            fixture.handle(fixture.request, {
              keyId,
              kernelRepo: null,
              kernelOrg: null,
              idempotencyPrincipal: principal,
            }),
        );
        assert.equal(response.status, 200);
        assert.equal(forwarded.accountId, fixture.expectedAccountId);
        assert.equal(forwarded.body?.prompt_cache_key, "stable-analytics-key");
        assert.equal(Object.prototype.hasOwnProperty.call(forwarded.body, "prompt_cache_options"), false);
        if (fixture.expectsCacheOptionsWarning) {
          assert.match(response.headers.get("x-uos-warning") ?? "", /prompt_cache_options_ignored/);
        } else {
          assert.doesNotMatch(response.headers.get("x-uos-warning") ?? "", /prompt_cache_options_ignored/);
        }
        assert.equal(getResponseTelemetry(response)?.affinityOutcome, fixture.expectedAffinityOutcome);
        assert.equal(getResponseTelemetry(response)?.promptCacheKeyPresent, true);
        assert.equal(getResponseTelemetry(response)?.cachedInputTokens, 1024);
        assert.equal(getResponseTelemetry(response)?.cacheWriteInputTokens, 512);

        const recordedAnalyticsEvents: Parameters<typeof recordPromptCacheAnalytics>[0][] = [];
        const logged = await withTerminalRequestLog(response, {
          route: fixture.route,
          startedAtMonotonicMs: performance.now(),
          requestId: `cache-analytics-${index}`,
          recordCacheAnalytics: (event) => {
            recordedAnalyticsEvents.push(event);
            return recordPromptCacheAnalytics(event, {
              kv: analyticsKv as unknown as Deno.Kv,
              release: "0123456789abcdef0123456789abcdef01234567",
              now: () => analyticsNow,
            });
          },
          recordTelemetry: () =>
            Promise.resolve({
              status: "ignored" as const,
              reason: "unknown_release" as const,
              release: null,
              provider: null,
              route: null,
              model_hash: null,
            }),
        });
        const recordedAnalyticsEvent = recordedAnalyticsEvents[0];
        assert.ok(recordedAnalyticsEvent);
        assert.deepEqual(
          {
            provider: recordedAnalyticsEvent.provider,
            model: recordedAnalyticsEvent.model,
            route: recordedAnalyticsEvent.route,
            promptCacheKeyPresent: recordedAnalyticsEvent.promptCacheKeyPresent,
            promptCacheMode: recordedAnalyticsEvent.promptCacheMode,
            fallbackReason: recordedAnalyticsEvent.fallbackReason,
          },
          {
            provider: "chatgpt_codex",
            model: DEFAULT_TEST_MODEL,
            route: fixture.route,
            promptCacheKeyPresent: true,
            promptCacheMode: fixture.expectedPromptCacheMode,
            fallbackReason: null,
          },
        );
        assert.equal("affinityOutcome" in recordedAnalyticsEvent, false);
        await logged.body?.cancel();
      }
    } finally {
      for (const affinityKey of affinityKeys) kvStore.delete(affinityKey);
      if (previousAuth === undefined) kvStore.delete(authKey);
      else kvStore.set(authKey, previousAuth);
      if (previousRouting === undefined) kvStore.delete(routingKey);
      else kvStore.set(routingKey, previousRouting);
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
    }

    const analytics = await readPromptCacheAnalytics({
      kv: analyticsKv as unknown as Deno.Kv,
      now: () => analyticsNow,
    });
    assert.deepEqual(analytics.buckets[0], {
      bucket_start_at_ms: analyticsNow,
      bucket_end_at_ms: analyticsNow + 15 * 60_000,
      input_tokens: 4096,
      cached_input_tokens: 2048,
      cache_write_input_tokens: 1024,
      cache_write_reported_sample_count: 2,
      cached_percentage: 50,
      sample_count: 2,
    });
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
      outputTokenAllowance: null,
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
      semanticOutputObserved: true,
      upstreamEventKinds: ["response.created", "response.completed"],
      streamTerminalType: "response.completed",
      failureKind: null,
      responseCreatedObserved: true,
      syntheticTerminalType: null,
      stream: false,
      providerRequestId: null,
      firstProviderDispatchMs: null,
      firstProviderHeadersMs: null,
      firstCodexDispatchMs: null,
      firstCodexHeadersMs: null,
      firstUpstreamSseEventMs: null,
      firstSemanticCommitmentMs: null,
      streamTerminalMs: null,
      attemptedProviders: ["chatgpt_codex"],
      removedProviderTriggerClass: null,
      removedProviderCircuitTransition: null,
      removedProviderSelectedModel: null,
      removedProviderTaskType: null,
      removedProviderSemanticCommitment: null,
      removedProviderLatencyMs: null,
      removedProviderTerminalStatus: null,
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
                  output: [{
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "partial usage output" }],
                  }],
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
                  output: [{
                    id: "msg_absent_cached_tokens",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
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
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [{
                    id: "msg_absent_usage",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
                },
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
                  output: [{
                    id: "msg_cache_read_above_input",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
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
                  output: [{
                    id: "msg_overlapping_cache_accounting",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
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
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [{
                    id: "msg_inconsistent_totals",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
                  usage: inconsistentUsage,
                },
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
                response: {
                  model: DEFAULT_TEST_MODEL,
                  output: [{
                    id: "msg_malformed_usage",
                    type: "message",
                    role: "assistant",
                    content: [{ type: "output_text", text: "done" }],
                  }],
                  usage: null,
                },
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

Deno.test("openai: accepts standard cache breakpoints but omits them from the Codex wire", async (t) => {
  await t.step("Responses preserves content while omitting breakpoints", async () => {
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
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(warnings.includes("prompt_cache_breakpoint_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as unknown as Record<string, unknown>;
    const content = ((recorded.input as Array<Record<string, unknown>>)[0]?.content ?? []) as Array<
      Record<string, unknown>
    >;
    assert.deepEqual(
      content.map((item) => item.prompt_cache_breakpoint),
      [undefined, undefined, undefined],
    );
    assert.deepEqual(content[2], {
      type: "input_file",
      file_id: "file_stable",
      detail: "high",
    });
  });

  await t.step("Responses preserves function-call output content and file detail", async () => {
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
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(warnings.includes("prompt_cache_breakpoint_ignored"));
    assert.ok(recordedBody);
    const input = (recordedBody as unknown as Record<string, unknown>).input as Array<Record<string, unknown>>;
    assert.deepEqual(input[0]?.output, [
      { type: "input_text", text: "stable tool result" },
      {
        type: "input_image",
        image_url: "https://example.test/tool-result.png",
      },
      {
        type: "input_file",
        file_id: "file_tool_result",
        detail: "low",
      },
    ]);
    assert.equal(getResponseTelemetry(response)?.explicitBreakpointCount, 3);
  });

  await t.step(
    "Chat preserves text/image and ordered developer input while omitting breakpoints",
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
      const warnings = parseWarnings(response.headers.get("x-uos-warning"));
      assert.ok(warnings.includes("prompt_cache_options_ignored"));
      assert.ok(warnings.includes("prompt_cache_breakpoint_ignored"));
      assert.ok(recordedBody);
      const recorded = recordedBody as unknown as Record<string, unknown>;
      assert.equal("instructions" in recorded, false);
      const input = recorded.input as Array<Record<string, unknown>>;
      assert.deepEqual(input.map((item) => item.role), ["developer", "developer", "user"]);
      const first = input[0]?.content as Array<Record<string, unknown>>;
      const last = input[2]?.content as Array<Record<string, unknown>>;
      assert.equal(first[0]?.prompt_cache_breakpoint, undefined);
      assert.deepEqual(last.map((item) => item.prompt_cache_breakpoint), [undefined, undefined]);
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

  await t.step("Chat preserves tool output while omitting its breakpoint", async () => {
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
    assert.ok(parseWarnings(response.headers.get("x-uos-warning")).includes("prompt_cache_breakpoint_ignored"));
    assert.equal(dispatches, 1);
    assert.ok(recordedBody);
    const input = (recordedBody as unknown as Record<string, unknown>).input as Array<Record<string, unknown>>;
    assert.deepEqual(input[0]?.output, [
      { type: "input_text", text: "stable tool result" },
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
    const warnings = parseWarnings(response.headers.get("x-uos-warning"));
    assert.ok(warnings.includes("prompt_cache_options_ignored"));
    assert.ok(warnings.includes("prompt_cache_breakpoint_ignored"));
    assert.ok(recordedBody);
    const recorded = recordedBody as Record<string, unknown>;
    assert.equal("instructions" in recorded, false);
    const input = recorded.input as Array<Record<string, unknown>>;
    assert.deepEqual(input.map((item) => item.role), ["developer", "user", "developer", "user"]);
    assert.deepEqual(input[0]?.content, [
      { type: "input_text", text: "stable system" },
    ]);
    assert.deepEqual(input[2]?.content, [{ type: "input_text", text: "stable developer" }]);
    assert.deepEqual(input[3]?.content, [
      { type: "input_text", text: "Read these files" },
      {
        type: "input_file",
        file_id: "file_stable",
        filename: "stable.txt",
      },
      {
        type: "input_file",
        file_data: "data:text/plain;base64,c3RhYmxl",
        filename: "inline.txt",
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
          { id: "metered", controls: controls({ key: false, explicit_breakpoints: false }) },
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

Deno.test("openai: buffered Responses preserve nested response.output items", async () => {
  const item = {
    id: "call_nested_output",
    type: "function_call",
    call_id: "call_nested_output",
    name: "lookup",
    arguments: "{}",
    status: "completed",
  };
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${JSON.stringify({ type: "response.output", response: { output: [item] } })}\n\n`,
        `data: ${JSON.stringify({ type: "response.completed", response: { output: [] } })}\n\n`,
      ]),
    () => handleResponses(responsesRequest({ stream: false })),
  );
  const payload = await response.json() as { output?: unknown[] };
  assert.equal(response.status, 200);
  assert.deepEqual(payload.output, [item]);
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

Deno.test("openai: Cerebras GPT-OSS Chat Completions adapter is native, bounded, and content-safe", async (t) => {
  const envKey = "CEREBRAS_API_KEY";
  const fakeApiKey = "cerebras-test-key";
  const originalApiKey = Deno.env.get(envKey);
  const restoreApiKey = (): void => {
    if (originalApiKey === undefined) Deno.env.delete(envKey);
    else Deno.env.set(envKey, originalApiKey);
  };
  const request = (body: Record<string, unknown>, signal?: AbortSignal): Request =>
    new Request("https://ai.ubq.fi/v1/chat/completions", {
      method: "POST",
      headers: { "Content-Type": "application/json", "x-uos-upstream": "chatgpt_codex" },
      body: JSON.stringify(body),
      signal,
    });
  const canonicalBody = {
    model: "gpt-oss-120b",
    messages: [
      { role: "developer", content: "Use exactly one function tool call." },
      { role: "user", content: "Prepare the dashboard summary." },
    ],
    tools: [{
      type: "function",
      function: {
        name: "assistant_message",
        description: "Return the assistant response envelope.",
        parameters: {
          type: "object",
          additionalProperties: false,
          properties: { message: { type: "string" } },
          required: ["message"],
        },
      },
    }],
    tool_choice: "required",
    parallel_tool_calls: false,
    reasoning_effort: "medium",
    temperature: 0,
    max_completion_tokens: 2048,
    stream: false,
  } as const;
  const refusalBody = {
    model: "gpt-oss-120b",
    messages: [{ role: "user", content: "Request content that the model must refuse." }],
    reasoning_effort: "medium",
    stream: false,
  } as const;
  const completionWithMessage = (id: string, message: Record<string, unknown>): Record<string, unknown> => ({
    id,
    object: "chat.completion",
    created: 1_728_000_006,
    model: "gpt-oss-120b",
    choices: [{ index: 0, message, finish_reason: "stop" }],
    usage: { prompt_tokens: 3, completion_tokens: 2, total_tokens: 5 },
  });

  Deno.env.set(envKey, fakeApiKey);
  try {
    await t.step("projects strict tools to Cerebras supported schema fields", () => {
      const projected = projectCerebrasToolSchema({
        type: "object",
        additionalProperties: false,
        properties: {
          message: {
            type: "string",
            minLength: 1,
            maxLength: 200,
            pattern: "^[A-Z]",
            format: "email",
          },
          references: {
            type: "array",
            minItems: 1,
            maxItems: 3,
            uniqueItems: true,
            items: {
              oneOf: [{ type: "object", properties: { source: { const: "view" } } }],
            },
          },
        },
        required: ["message"],
      });
      assert.deepEqual(projected, {
        type: "object",
        additionalProperties: false,
        properties: {
          message: { type: "string" },
          references: {
            type: "array",
            items: {
              anyOf: [{ type: "object", properties: { source: { enum: ["view"] } } }],
            },
          },
        },
        required: ["message"],
      });

      assert.deepEqual(
        projectCerebrasToolSchema({
          oneOf: [
            {
              type: "object",
              properties: {
                operationId: { const: "briefings.daily" },
                arguments: { type: "object", properties: {}, additionalProperties: false },
                references: { type: "array", items: { type: "string" } },
              },
              required: ["operationId", "arguments", "references"],
              additionalProperties: false,
            },
            {
              type: "object",
              properties: {
                operationId: { const: "campaigns.summary" },
                arguments: {
                  type: "object",
                  properties: { campaignId: { type: "string", minLength: 1 } },
                  required: ["campaignId"],
                  additionalProperties: false,
                },
                references: { type: "array", items: { type: "string" } },
              },
              required: ["operationId", "arguments", "references"],
              additionalProperties: false,
            },
          ],
        }),
        {
          type: "object",
          properties: {
            operationId: { enum: ["briefings.daily", "campaigns.summary"] },
            arguments: {
              anyOf: [
                { type: "object", properties: {}, additionalProperties: false },
                {
                  type: "object",
                  properties: { campaignId: { type: "string" } },
                  required: ["campaignId"],
                  additionalProperties: false,
                },
              ],
            },
            references: { type: "array", items: { type: "string" } },
          },
          required: ["arguments", "operationId", "references"],
          additionalProperties: false,
        },
      );

      assert.deepEqual(
        projectCerebrasToolSchema({
          oneOf: [
            {
              type: "object",
              properties: {
                operationId: { const: "search" },
                query: { type: "string" },
              },
              required: ["operationId", "query"],
              additionalProperties: false,
            },
            {
              type: "object",
              properties: {
                operationId: { const: "read" },
                documentId: { type: "string" },
              },
              required: ["operationId", "documentId"],
              additionalProperties: false,
            },
          ],
        }),
        {
          type: "object",
          properties: {
            operationId: { enum: ["search", "read"] },
            query: { type: "string" },
            documentId: { type: "string" },
          },
          required: ["operationId"],
          additionalProperties: false,
        },
      );
    });

    await t.step("never drops property names that collide with schema keywords", () => {
      // D3 regression (gateway commit): a property literally named `pattern`
      // (or format/minLength/...) was deleted by the keyword projection while
      // `required` still named it, so the model "misassigned" arguments into
      // whatever field survived. Property NAMES are not schema keywords.
      const projected = projectCerebrasToolSchema({
        type: "object",
        properties: {
          pattern: { type: "string", description: "Glob to match files, e.g. *.py" },
          path: { type: "string", description: "Directory to search (optional)." },
          format: { type: "string", description: "A field named format." },
        },
        required: ["pattern"],
        additionalProperties: false,
      });
      assert.deepEqual((projected as Record<string, unknown>).properties, {
        pattern: { type: "string", description: "Glob to match files, e.g. *.py" },
        path: { type: "string", description: "Directory to search (optional)." },
        format: { type: "string", description: "A field named format." },
      });
      // Keyword FIELDS inside property schemas are still stripped for Cerebras.
      assert.deepEqual(
        projectCerebrasToolSchema({
          type: "object",
          properties: { glob: { type: "string", pattern: "^[A-Z]", minLength: 1 } },
          required: ["glob"],
        }),
        {
          type: "object",
          properties: { glob: { type: "string" } },
          required: ["glob"],
        },
      );
    });

    await t.step("preserves a property named like a keyword anywhere in the graph", () => {
      const projected = projectCerebrasToolSchema({
        type: "object",
        properties: {
          oneOf: { type: "string" }, // property NAMED oneOf stays a property
          nested: {
            type: "object",
            properties: { uniqueItems: { type: "number" } },
          },
        },
        required: ["oneOf"],
      }) as { properties: Record<string, unknown> };
      assert.equal(typeof projected.properties.oneOf, "object");
      const nestedProps =
        (projected.properties.nested as { properties: Record<string, { type?: unknown }> }).properties;
      assert.equal(nestedProps.uniqueItems?.type, "number");
    });

    await t.step("routes the exact model and preserves native tools/tool choice", async () => {
      const upstreamCalls: Array<{ url: string; body: Record<string, unknown>; headers: Headers }> = [];
      const logs: unknown[][] = [];
      const originalError = console.error;
      console.error = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          (url, bodyText, init) => {
            upstreamCalls.push({
              url,
              body: bodyText ? JSON.parse(bodyText) as Record<string, unknown> : {},
              headers: new Headers(init?.headers),
            });
            return new Response(
              JSON.stringify({
                id: "chatcmpl_cerebras_fixture",
                object: "chat.completion",
                created: 1_728_000_000,
                model: "gpt-oss-120b",
                choices: [{
                  index: 0,
                  message: {
                    role: "assistant",
                    content: null,
                    tool_calls: [{
                      id: "call_fixture",
                      type: "function",
                      provider_trace: "provider-tool-field-must-not-be-relayed",
                      function: { name: "assistant_message", arguments: '{"message":"Ready"}' },
                    }],
                  },
                  finish_reason: "tool_calls",
                }],
                usage: { prompt_tokens: 13, completion_tokens: 7, total_tokens: 20 },
                provider_debug: "provider-body-must-not-be-logged-or-relayed",
              }),
              {
                status: 200,
                headers: {
                  "Content-Type": "application/json",
                  "X-Request-Id": "cerebras-header-request-1",
                },
              },
            );
          },
          () =>
            handleChatCompletions(
              request(canonicalBody),
              {
                keyId: null,
                kernelRepo: null,
                kernelOrg: null,
                requestId: "cerebras-success",
                startedAtMs: Date.now(),
                startedAtMonotonicMs: performance.now(),
              },
            ),
        );

        assert.equal(response.status, 200);
        assert.deepEqual(upstreamCalls.map((call) => call.url), ["https://api.cerebras.ai/v1/chat/completions"]);
        assert.deepEqual(upstreamCalls[0]?.body, canonicalBody);
        assert.equal(upstreamCalls[0]?.headers.get("Authorization"), `Bearer ${fakeApiKey}`);
        assert.equal(upstreamCalls[0]?.headers.get("Content-Type"), "application/json");
        const payload = await response.json() as {
          model?: string;
          choices?: Array<{ message?: { tool_calls?: Array<Record<string, unknown>> } }>;
          usage?: Record<string, unknown>;
          provider_debug?: unknown;
        };
        assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
        assert.equal(response.headers.get("x-uos-provider-request-id"), "cerebras-header-request-1");
        assert.equal(payload.model, "gpt-oss-120b");
        assert.equal(payload.provider_debug, undefined);
        assert.deepEqual(payload.choices?.[0]?.message?.tool_calls, [{
          id: "call_fixture",
          type: "function",
          function: { name: "assistant_message", arguments: '{"message":"Ready"}' },
        }]);
        assert.deepEqual(payload.usage, { prompt_tokens: 13, completion_tokens: 7, total_tokens: 20 });
        const telemetry = getResponseTelemetry(response);
        assert.equal(telemetry?.provider, "cerebras");
        assert.equal(telemetry?.providerRequestId, "cerebras-header-request-1");
        assert.equal(telemetry?.inputTokens, 13);
        assert.equal(telemetry?.outputTokens, 7);
        assert.equal(telemetry?.completed, true);
        assert.equal(telemetry?.stream, false);
        assert.deepEqual(telemetry?.attemptedProviders, ["cerebras"]);
        assert.equal(telemetry?.failureKind, null);
        assert.equal(typeof telemetry?.firstProviderDispatchMs, "number");
        assert.equal(typeof telemetry?.firstProviderHeadersMs, "number");
        const logText = JSON.stringify(logs);
        assert.doesNotMatch(logText, /cerebras-test-key/);
        assert.doesNotMatch(logText, /provider-body-must-not-be-logged-or-relayed/);
      } finally {
        console.error = originalError;
      }
    });

    await t.step("preserves upstream reasoning 1:1 in buffered Chat responses", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_reasoning_buffered", {
                role: "assistant",
                content: "pong",
                reasoning: "the model considered the ping before answering pong.",
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request({ ...refusalBody, stream: false })),
      );

      assert.equal(response.status, 200);
      const payload = await response.json() as {
        choices?: Array<{ message?: Record<string, unknown> }>;
      };
      assert.equal(payload.choices?.[0]?.message?.role, "assistant");
      assert.equal(payload.choices?.[0]?.message?.content, "pong");
      assert.equal(
        payload.choices?.[0]?.message?.reasoning,
        "the model considered the ping before answering pong.",
      );
    });

    await t.step("preserves upstream reasoning 1:1 in downgraded Chat streams", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_reasoning_stream", {
                role: "assistant",
                content: null,
                reasoning: "streamed reasoning trace.",
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request({ ...refusalBody, stream: true })),
      );

      assert.equal(response.status, 200);
      const streamText = await response.text();
      const firstDataLine = streamText.split("\n").find((line) => line.startsWith("data: {"));
      assert.ok(firstDataLine);
      const firstEvent = JSON.parse(firstDataLine.slice("data: ".length)) as {
        choices?: Array<{ delta?: Record<string, unknown> }>;
      };
      // Native mirror: reasoning rides the leading delta (content stays in
      // the same chunk when present; here the fixture has content: null).
      assert.equal(firstEvent.choices?.[0]?.delta?.role, "assistant");
      assert.equal(firstEvent.choices?.[0]?.delta?.reasoning, "streamed reasoning trace.");
      assert.match(streamText, /data: \[DONE\]/);
    });

    await t.step("forwards bounded upstream error message/code 1:1", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify({
              message:
                "Tools with mixed values for 'strict' are not allowed. Please set all tools to 'strict: true' or 'strict: false'",
              type: "invalid_request_error",
              param: "tools",
              code: "wrong_api_format",
            }),
            {
              status: 400,
              headers: {
                "Content-Type": "application/json",
                "X-Request-Id": "cerebras-error-request-1",
                "Retry-After": "17",
              },
            },
          ),
        () => handleChatCompletions(request(refusalBody)),
      );

      assert.equal(response.status, 400);
      assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
      assert.equal(response.headers.get("x-uos-provider-request-id"), "cerebras-error-request-1");
      assert.equal(response.headers.get("Retry-After"), "17");
      const error = (await response.json() as {
        error?: { message?: string; code?: string; type?: string };
      }).error;
      assert.equal(
        error?.message,
        "Tools with mixed values for 'strict' are not allowed. Please set all tools to 'strict: true' or 'strict: false'",
      );
      assert.equal(error?.code, "wrong_api_format");
      assert.equal(error?.type, "invalid_request_error");
    });

    await t.step("keeps the generic error when the upstream body is not JSON", async () => {
      const response = await withFetchMock(
        () =>
          new Response("<html>provider diagnostic page</html>", {
            status: 502,
            headers: { "Content-Type": "text/html" },
          }),
        () => handleChatCompletions(request(refusalBody)),
      );

      assert.equal(response.status, 502);
      const error = (await response.json() as {
        error?: { message?: string; code?: string };
      }).error;
      assert.equal(error?.message, "Cerebras upstream returned an error.");
      assert.equal(error?.code, "cerebras_upstream_error");
    });

    await t.step("preserves provider-native refusals in buffered Chat responses", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_refusal_buffered", {
                role: "assistant",
                content: "I cannot provide those instructions.",
                refusal: "The request conflicts with safety policy.",
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request(refusalBody)),
      );

      assert.equal(response.status, 200);
      const payload = await response.json() as {
        choices?: Array<{ message?: Record<string, unknown> }>;
      };
      assert.deepEqual(payload.choices?.[0]?.message, {
        role: "assistant",
        content: "I cannot provide those instructions.",
        refusal: "The request conflicts with safety policy.",
      });
    });

    await t.step("emits provider-native refusals in downgraded Chat streams", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_refusal_stream", {
                role: "assistant",
                content: null,
                refusal: "I cannot help with that.",
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request({ ...refusalBody, stream: true })),
      );

      assert.equal(response.status, 200);
      const streamText = await response.text();
      const firstDataLine = streamText.split("\n").find((line) => line.startsWith("data: {"));
      assert.ok(firstDataLine);
      const firstEvent = JSON.parse(firstDataLine.slice("data: ".length)) as {
        choices?: Array<{ delta?: Record<string, unknown> }>;
      };
      assert.deepEqual(firstEvent.choices?.[0]?.delta, {
        role: "assistant",
        refusal: "I cannot help with that.",
      });
      assert.match(streamText, /data: \[DONE\]/);
    });

    await t.step("rejects non-string provider-native refusals", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_invalid_refusal", {
                role: "assistant",
                content: null,
                refusal: { reason: "policy" },
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request(refusalBody)),
      );

      assert.equal(response.status, 502);
      assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
      assert.equal(
        (await response.json() as { error?: { code?: string } }).error?.code,
        "cerebras_upstream_invalid_response",
      );
    });

    await t.step("counts a refusal-only completion as semantic output", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify(
              completionWithMessage("chatcmpl_cerebras_refusal_only", {
                role: "assistant",
                content: null,
                refusal: "I cannot comply.",
              }),
            ),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request(refusalBody)),
      );

      assert.equal(response.status, 200);
      const payload = await response.json() as {
        choices?: Array<{ message?: Record<string, unknown> }>;
      };
      assert.deepEqual(payload.choices?.[0]?.message, {
        role: "assistant",
        content: null,
        refusal: "I cannot comply.",
      });
      const telemetry = getResponseTelemetry(response);
      assert.equal(telemetry?.semanticOutputObserved, true);
      assert.equal(telemetry?.completed, true);
    });

    await t.step("keeps reading a valid buffered body past the error-body deadline", async () => {
      const delayedPayload = JSON.stringify({
        id: "chatcmpl_cerebras_delayed_body",
        object: "chat.completion",
        created: 1_728_000_000,
        model: "gpt-oss-120b",
        choices: [{
          index: 0,
          message: { role: "assistant", content: "Ready" },
          finish_reason: "stop",
        }],
        usage: { prompt_tokens: 2, completion_tokens: 1, total_tokens: 3 },
      });
      const response = await withFetchMock(
        () =>
          new Response(
            new ReadableStream<Uint8Array>({
              async start(controller) {
                await new Promise((resolve) => setTimeout(resolve, 1_050));
                controller.enqueue(TEXT_ENCODER.encode(delayedPayload));
                controller.close();
              },
            }),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request(canonicalBody)),
      );
      assert.equal(response.status, 200);
      assert.equal((await response.json() as { id?: string }).id, "chatcmpl_cerebras_delayed_body");
    });

    await t.step("does not route similarly named models to Cerebras", async () => {
      let cerebrasCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.cerebras.ai/v1/chat/completions") cerebrasCalls += 1;
          return sseResponse(baseSseChunks());
        },
        () => handleChatCompletions(request({ ...canonicalBody, model: "gpt-oss-120b-preview" })),
      );
      assert.notEqual(response.status, 200);
      assert.equal(cerebrasCalls, 0);
    });

    await t.step("rejects none reasoning locally without provider dispatch", async () => {
      let cerebrasCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://api.cerebras.ai/v1/chat/completions") cerebrasCalls += 1;
          return new Response("{}", { status: 200 });
        },
        () =>
          handleChatCompletions(
            request({ ...canonicalBody, reasoning_effort: "none" }),
            {
              keyId: null,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "cerebras-none-validation",
              startedAtMs: Date.now(),
              startedAtMonotonicMs: performance.now(),
            },
          ),
      );

      assert.equal(response.status, 400);
      const payload = await response.json() as {
        error?: { code?: string; message?: string; param?: string; type?: string };
      };
      assert.equal(payload.error?.type, "invalid_request_error");
      assert.equal(payload.error?.code, "invalid_request_error");
      assert.equal(payload.error?.param, "reasoning_effort");
      assert.match(payload.error?.message ?? "", /none.*low.*medium.*high/i);
      assert.equal(cerebrasCalls, 0);
      assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, []);
    });

    await t.step("defaults omitted reasoning to medium without converting native Chat fields", async () => {
      const { reasoning_effort: _reasoningEffort, ...withoutReasoning } = canonicalBody;
      const upstreamBodies: Record<string, unknown>[] = [];
      const response = await withFetchMock(
        (_url, bodyText) => {
          if (bodyText) upstreamBodies.push(JSON.parse(bodyText) as Record<string, unknown>);
          return new Response(
            JSON.stringify({
              id: "chatcmpl_cerebras_default_reasoning",
              object: "chat.completion",
              created: 1_728_000_001,
              model: "gpt-oss-120b",
              choices: [{
                index: 0,
                message: { role: "assistant", content: "Ready" },
                finish_reason: "stop",
              }],
              usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 },
            }),
            { status: 200, headers: { "Content-Type": "application/json" } },
          );
        },
        () => handleChatCompletions(request(withoutReasoning)),
      );
      assert.equal(response.status, 200);
      assert.equal(upstreamBodies.length, 1);
      const upstreamBody = upstreamBodies[0]!;
      assert.equal(upstreamBody.reasoning_effort, "medium");
      assert.deepEqual(upstreamBody.tools, canonicalBody.tools);
      assert.equal(upstreamBody.tool_choice, canonicalBody.tool_choice);
      assert.equal(upstreamBody.stream, false);
    });

    await t.step("downgrades Chat Completions streaming while keeping Responses unavailable", async () => {
      let cerebrasCalls = 0;
      const upstreamBodies: Record<string, unknown>[] = [];
      const streamResponse = await withFetchMock(
        (url, bodyText) => {
          if (url !== "https://api.cerebras.ai/v1/chat/completions") throw new Error(`unexpected URL: ${url}`);
          cerebrasCalls += 1;
          if (bodyText) upstreamBodies.push(JSON.parse(bodyText) as Record<string, unknown>);
          return new Response(
            JSON.stringify({
              id: "chatcmpl_cerebras_buffered_stream",
              object: "chat.completion",
              created: 1_728_000_004,
              model: "gpt-oss-120b",
              choices: [{
                index: 0,
                message: { role: "assistant", content: "Ready" },
                finish_reason: "stop",
              }],
              usage: { prompt_tokens: 3, completion_tokens: 1, total_tokens: 4 },
            }),
            { status: 200, headers: { "Content-Type": "application/json" } },
          );
        },
        () =>
          handleChatCompletions(
            request({ ...canonicalBody, stream: true, stream_options: { include_usage: true } }),
            {
              keyId: null,
              kernelRepo: null,
              kernelOrg: null,
              requestId: "cerebras-buffered-stream-telemetry",
              startedAtMs: Date.now(),
              startedAtMonotonicMs: performance.now(),
            },
          ),
      );
      assert.equal(streamResponse.status, 200);
      assert.equal(streamResponse.headers.get("Content-Type"), "text/event-stream");
      assert.equal(streamResponse.headers.get("x-uos-warning"), "gpt_oss_stream_downgraded");
      const telemetry = getResponseTelemetry(streamResponse);
      assert.ok(telemetry);
      assert.equal(telemetry.firstUpstreamSseEventMs, null);
      assert.equal(typeof telemetry.firstSemanticCommitmentMs, "number");
      assert.equal(typeof telemetry.streamTerminalMs, "number");
      const streamText = await streamResponse.text();
      assert.match(streamText, /"object":"chat\.completion\.chunk"/);
      assert.match(streamText, /"content":"Ready"/);
      assert.match(streamText, /"usage":\{"prompt_tokens":3,"completion_tokens":1,"total_tokens":4\}/);
      assert.match(streamText, /data: \[DONE\]/);
      const upstreamBody = upstreamBodies[0]!;
      assert.equal(upstreamBody.stream, false);
      assert.equal(upstreamBody.stream_options, undefined);
      assert.equal(cerebrasCalls, 1);

      const responsesResponse = await withFetchMock(
        (url) => {
          if (url === "https://api.cerebras.ai/v1/chat/completions") cerebrasCalls += 1;
          return sseResponse(baseSseChunks());
        },
        () =>
          handleResponses(
            new Request("https://ai.ubq.fi/v1/responses", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ model: "gpt-oss-120b", input: "ping" }),
            }),
          ),
      );
      assert.equal(responsesResponse.status, 400);
      assert.equal((await responsesResponse.json() as { error?: { param?: string } }).error?.param, "model");
      assert.equal(cerebrasCalls, 1);
    });

    await t.step("rejects a missing server credential without provider dispatch", async () => {
      Deno.env.delete(envKey);
      try {
        let cerebrasCalls = 0;
        const response = await withFetchMock(
          (url) => {
            if (url === "https://api.cerebras.ai/v1/chat/completions") cerebrasCalls += 1;
            return sseResponse(baseSseChunks());
          },
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(response.status, 503);
        assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
        assert.equal((await response.json() as { error?: { code?: string } }).error?.code, "cerebras_api_key_missing");
        assert.equal(getResponseTelemetry(response)?.failureKind, "cerebras_api_key_missing");
        assert.equal(cerebrasCalls, 0);
      } finally {
        Deno.env.set(envKey, fakeApiKey);
      }
    });

    await t.step("normalizes upstream 400/401/408/429/5xx without logging provider bodies", async () => {
      const cases = [
        { status: 400, expectedType: "invalid_request_error" },
        { status: 401, expectedType: "invalid_request_error" },
        { status: 408, expectedType: "server_error" },
        { status: 429, expectedType: "rate_limit_error" },
        { status: 503, expectedType: "server_error" },
      ] as const;
      const cerebrasRateLimitHeaders = {
        "x-ratelimit-limit-requests-minute": "5",
        "x-ratelimit-remaining-requests-minute": "0",
        "x-ratelimit-reset-requests-minute": "42",
        "x-ratelimit-limit-tokens-minute": "30000",
        "x-ratelimit-remaining-tokens-minute": "29999",
        "x-ratelimit-reset-tokens-minute": "42",
        "x-ratelimit-limit-requests-day": "1000",
        "x-ratelimit-remaining-requests-day": "999",
        "x-ratelimit-reset-requests-day": "86400",
        "x-ratelimit-limit-tokens-day": "1000000",
        "x-ratelimit-remaining-tokens-day": "999999",
        "x-ratelimit-reset-tokens-day": "86400",
      };
      for (const testCase of cases) {
        const logs: unknown[][] = [];
        const originalError = console.error;
        console.error = (...args: unknown[]) => logs.push(args);
        try {
          const response = await withFetchMock(
            (url) => {
              assert.equal(url, "https://api.cerebras.ai/v1/chat/completions");
              return new Response(
                JSON.stringify({
                  error: {
                    message: "provider-body-must-not-be-logged-or-relayed",
                    code: "fixture_failure",
                    // Arbitrary provider diagnostics must NEVER be relayed:
                    // only the standard bounded message/code fields are.
                    provider_debug_marker: "provider-body-must-not-be-logged-or-relayed",
                    trace_id: "provider-trace-must-not-be-relayed",
                  },
                }),
                {
                  status: testCase.status,
                  headers: {
                    "Content-Type": "application/json",
                    "X-Request-Id": `cerebras-error-${testCase.status}`,
                    ...(testCase.status === 429 ? { "Retry-After": "17" } : {}),
                    ...cerebrasRateLimitHeaders,
                  },
                },
              );
            },
            () =>
              handleChatCompletions(request(canonicalBody), {
                keyId: null,
                kernelRepo: null,
                kernelOrg: null,
                requestId: `cerebras-error-${testCase.status}`,
                startedAtMs: Date.now(),
                startedAtMonotonicMs: performance.now(),
              }),
          );
          assert.equal(response.status, testCase.status);
          assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
          assert.equal(response.headers.get("x-uos-provider-request-id"), `cerebras-error-${testCase.status}`);
          assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["cerebras"]);
          assert.equal(response.headers.get("Retry-After"), testCase.status === 429 ? "17" : null);
          for (const [header, value] of Object.entries(cerebrasRateLimitHeaders)) {
            assert.equal(
              response.headers.get(header),
              testCase.status === 429 ? value : null,
              `${header} on ${testCase.status}`,
            );
          }
          const payload = await response.json() as {
            error?: {
              message?: string;
              type?: string;
              code?: string;
              provider_debug_marker?: string;
              trace_id?: string;
            };
          };
          assert.equal(payload.error?.type, testCase.expectedType);
          // D2 (2026-08-29): bounded standard upstream fields ARE forwarded 1:1.
          assert.equal(payload.error?.code, "fixture_failure");
          assert.equal(payload.error?.message, "provider-body-must-not-be-logged-or-relayed");
          // The body-reflection safety property still holds: unknown provider
          // fields never reach the client.
          assert.equal(payload.error?.provider_debug_marker, undefined);
          assert.equal(payload.error?.trace_id, undefined);
          assert.equal(getResponseTelemetry(response)?.failureKind, "upstream_http_error");
          const logText = JSON.stringify(logs);
          assert.doesNotMatch(logText, /provider-body-must-not-be-logged-or-relayed/);
          assert.doesNotMatch(logText, /cerebras-test-key/);
        } finally {
          console.error = originalError;
        }
      }
    });

    await t.step("rejects malformed tool-call output without reflecting provider content", async () => {
      const response = await withFetchMock(
        () =>
          new Response(
            JSON.stringify({
              id: "chatcmpl_cerebras_invalid_tool",
              object: "chat.completion",
              created: 1_728_000_002,
              model: "gpt-oss-120b",
              choices: [{
                index: 0,
                message: {
                  role: "assistant",
                  content: null,
                  tool_calls: [{
                    id: "call_invalid",
                    type: "function",
                    function: {
                      name: "assistant_message",
                      arguments: { marker: "provider-body-must-not-be-relayed" },
                    },
                  }],
                },
              }],
              usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 },
            }),
            { status: 200, headers: { "Content-Type": "application/json" } },
          ),
        () => handleChatCompletions(request(canonicalBody)),
      );
      assert.equal(response.status, 502);
      assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
      const payload = await response.json() as { error?: { code?: string; message?: string } };
      assert.equal(payload.error?.code, "cerebras_upstream_invalid_response");
      assert.equal(getResponseTelemetry(response)?.failureKind, "invalid_completion_schema");
      assert.doesNotMatch(payload.error?.message ?? "", /provider-body-must-not-be-relayed/);
    });

    await t.step("rejects missing or rewritten native tool-call fields", async () => {
      const malformedCalls = [
        {
          id: "call_missing_type",
          function: { name: "assistant_message", arguments: "{}" },
        },
        {
          id: " call_with_whitespace",
          type: "function",
          function: { name: "assistant_message", arguments: "{}" },
        },
        {
          id: "call_name_whitespace",
          type: "function",
          function: { name: " assistant_message", arguments: "{}" },
        },
      ];
      for (const toolCall of malformedCalls) {
        const response = await withFetchMock(
          () =>
            new Response(
              JSON.stringify({
                id: "chatcmpl_cerebras_invalid_native_tool",
                object: "chat.completion",
                created: 1_728_000_003,
                model: "gpt-oss-120b",
                choices: [{
                  index: 0,
                  message: { role: "assistant", content: null, tool_calls: [toolCall] },
                  finish_reason: "tool_calls",
                }],
                usage: { prompt_tokens: 1, completion_tokens: 1, total_tokens: 2 },
              }),
              { status: 200, headers: { "Content-Type": "application/json" } },
            ),
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(response.status, 502);
        assert.equal(
          (await response.json() as { error?: { code?: string } }).error?.code,
          "cerebras_upstream_invalid_response",
        );
        assert.equal(getResponseTelemetry(response)?.failureKind, "invalid_completion_schema");
      }
    });

    await t.step(
      "classifies transport, incomplete-body, and invalid-JSON failures without provider content",
      async () => {
        const unreachable = await withFetchMock(
          () => {
            throw new TypeError("provider transport detail must not be exposed");
          },
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(unreachable.status, 502);
        assert.equal(getResponseTelemetry(unreachable)?.streamTerminalType, "error");
        assert.equal(getResponseTelemetry(unreachable)?.failureKind, "upstream_unreachable");

        const incomplete = await withFetchMock(
          () =>
            new Response(
              new ReadableStream<Uint8Array>({
                start(controller) {
                  controller.enqueue(TEXT_ENCODER.encode('{"partial":'));
                  controller.error(new Error("provider body detail must not be exposed"));
                },
              }),
              { status: 200, headers: { "Content-Type": "application/json" } },
            ),
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(incomplete.status, 502);
        assert.equal(getResponseTelemetry(incomplete)?.streamTerminalType, "error");
        assert.equal(getResponseTelemetry(incomplete)?.failureKind, "incomplete_response");

        const invalidJson = await withFetchMock(
          () => new Response('{"invalid":', { status: 200, headers: { "Content-Type": "application/json" } }),
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(invalidJson.status, 502);
        assert.equal(getResponseTelemetry(invalidJson)?.streamTerminalType, "error");
        assert.equal(getResponseTelemetry(invalidJson)?.failureKind, "invalid_json");
      },
    );

    await t.step("bounds a pre-header timeout and forwards downstream cancellation", async () => {
      setCerebrasFetchTimeoutMsForTest(10);
      try {
        let timeoutCalls = 0;
        const timeoutResponse = await withFetchMock(
          (url, _body, init) => {
            timeoutCalls += 1;
            assert.equal(url, "https://api.cerebras.ai/v1/chat/completions");
            return new Promise<Response>((_resolve, reject) => {
              const signal = init?.signal;
              if (!signal) {
                reject(new Error("Cerebras request did not receive a cancellation signal"));
                return;
              }
              const rejectWithReason = () => reject(signal.reason);
              if (signal.aborted) rejectWithReason();
              else signal.addEventListener("abort", rejectWithReason, { once: true });
            });
          },
          () => handleChatCompletions(request(canonicalBody)),
        );
        assert.equal(timeoutCalls, 1);
        assert.equal(timeoutResponse.status, 504);
        assert.equal(timeoutResponse.headers.get("x-uos-upstream"), "cerebras");
        assert.equal((await timeoutResponse.json() as { error?: { code?: string } }).error?.code, "gateway_timeout");
        assert.equal(getResponseTelemetry(timeoutResponse)?.streamTerminalType, "deadline");
        assert.equal(getResponseTelemetry(timeoutResponse)?.failureKind, "deadline");
      } finally {
        setCerebrasFetchTimeoutMsForTest(null);
      }

      const controller = new AbortController();
      let downstreamAbortObserved = false;
      let cancellationCalls = 0;
      const cancelledResponse = await withFetchMock(
        (url, _body, init) => {
          cancellationCalls += 1;
          assert.equal(url, "https://api.cerebras.ai/v1/chat/completions");
          return new Promise<Response>((_resolve, reject) => {
            const signal = init?.signal;
            if (!signal) {
              reject(new Error("Cerebras request did not receive a cancellation signal"));
              return;
            }
            const rejectWithReason = () => {
              downstreamAbortObserved = true;
              reject(signal.reason);
            };
            signal.addEventListener("abort", rejectWithReason, { once: true });
            controller.abort(new DOMException("client disconnected", "AbortError"));
          });
        },
        () => handleChatCompletions(request(canonicalBody, controller.signal)),
      );
      assert.equal(cancellationCalls, 1);
      assert.equal(downstreamAbortObserved, true);
      assert.equal(cancelledResponse.status, 499);
      assert.equal(cancelledResponse.headers.get("x-uos-upstream"), "cerebras");
      assert.equal(getResponseTelemetry(cancelledResponse)?.streamTerminalType, "cancelled");
      assert.equal(getResponseTelemetry(cancelledResponse)?.failureKind, "cancellation");
    });

    await t.step("maps downstream cancellation while draining a buffered body to 499", async () => {
      const controller = new AbortController();
      let signalSecondRead: (() => void) | null = null;
      const secondReadStarted = new Promise<void>((resolve) => {
        signalSecondRead = resolve;
      });
      let upstreamCancelled = false;
      const response = await withFetchMock(
        () => {
          let emittedPartialChunk = false;
          return new Response(
            new ReadableStream<Uint8Array>({
              pull(streamController) {
                if (!emittedPartialChunk) {
                  emittedPartialChunk = true;
                  streamController.enqueue(TEXT_ENCODER.encode('{"partial":'));
                  return;
                }
                signalSecondRead?.();
                return new Promise<void>(() => {});
              },
              cancel() {
                upstreamCancelled = true;
              },
            }),
            {
              status: 200,
              headers: {
                "Content-Type": "application/json",
                "X-Request-Id": "cerebras-body-cancel-request",
              },
            },
          );
        },
        async () => {
          const pending = handleChatCompletions(request(canonicalBody, controller.signal));
          await secondReadStarted;
          controller.abort(new DOMException("client disconnected", "AbortError"));
          return await pending;
        },
      );

      assert.equal(response.status, 499);
      assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
      assert.equal(response.headers.get("x-uos-provider-request-id"), "cerebras-body-cancel-request");
      assert.equal((await response.json() as { error?: { code?: string } }).error?.code, "request_cancelled");
      assert.equal(getResponseTelemetry(response)?.streamTerminalType, "cancelled");
      assert.equal(getResponseTelemetry(response)?.failureKind, "cancellation");
      assert.equal(upstreamCancelled, true);
    });
  } finally {
    restoreApiKey();
    setCerebrasFetchTimeoutMsForTest(null);
  }
});

Deno.test("openai: oversized Responses events retain their redacted failure classification", async () => {
  const response = await withFetchMock(
    () => sseResponse([`data: ${"x".repeat(MAX_RESPONSES_SSE_EVENT_BYTES + 1)}\n\n`]),
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
        }),
      ),
  );
  assert.equal(response.status, 502);
  assert.equal(getResponseTelemetry(response)?.failureKind, "event_too_large");
  assert.equal(getResponseTelemetry(response)?.responseCreatedObserved, false);
  assert.equal(getResponseTelemetry(response)?.syntheticTerminalType, null);
});

Deno.test("openai: precommit telemetry records response.created before a malformed event", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_precommit_malformed" } })}\n\n`,
        'data: {"type":\n\n',
      ]),
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: DEFAULT_TEST_MODEL, input: "ping", stream: true }),
        }),
        {
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          requestId: "responses-precommit-telemetry",
          startedAtMs: Date.now(),
          startedAtMonotonicMs: performance.now(),
        },
      ),
  );
  assert.equal(response.status, 502);
  const telemetry = getResponseTelemetry(response);
  assert.ok(telemetry);
  assert.equal(telemetry.failureKind, "malformed_event");
  assert.equal(telemetry.responseCreatedObserved, true);
  assert.equal(telemetry.syntheticTerminalType, null);
  assert.equal(typeof telemetry.firstUpstreamSseEventMs, "number");
  assert.equal(telemetry.firstSemanticCommitmentMs, null);
  assert.equal(typeof telemetry.streamTerminalMs, "number");
  assert.ok(telemetry.firstUpstreamSseEventMs! <= telemetry.streamTerminalMs!);
});

Deno.test("openai: Chat precommit telemetry separates upstream arrival from semantic commitment", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        `data: ${JSON.stringify({ type: "response.created", response: { id: "chat_precommit_malformed" } })}\n\n`,
        'data: {"type":\n\n',
      ]),
    () =>
      handleChatCompletions(
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
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          requestId: "chat-precommit-telemetry",
          startedAtMs: Date.now(),
          startedAtMonotonicMs: performance.now(),
        },
      ),
  );
  assert.equal(response.status, 502);
  const telemetry = getResponseTelemetry(response);
  assert.ok(telemetry);
  assert.equal(telemetry.failureKind, "malformed_event");
  assert.equal(typeof telemetry.firstUpstreamSseEventMs, "number");
  assert.equal(telemetry.firstSemanticCommitmentMs, null);
  assert.equal(typeof telemetry.streamTerminalMs, "number");
  assert.ok(telemetry.firstUpstreamSseEventMs! <= telemetry.streamTerminalMs!);
});

Deno.test("openai: real Responses failure terminals classify without provider content", async (t) => {
  const secretMessage = "provider error message must not enter telemetry";
  const secretProviderField = "provider private diagnostic must not enter telemetry";
  const cases = [
    {
      name: "error",
      terminal: {
        type: "error",
        error: {
          code: "provider_error",
          type: "provider_error_type",
          message: secretMessage,
          provider_detail: secretProviderField,
        },
      },
      expectedFailureKind: "provider_error",
      expectedTerminalType: "error",
    },
    {
      name: "response.failed",
      terminal: {
        type: "response.failed",
        response: {
          id: "resp_real_failure",
          status: "failed",
          error: {
            type: "provider_failure",
            message: secretMessage,
            provider_detail: secretProviderField,
          },
          output: [],
        },
      },
      expectedFailureKind: "provider_failure",
      expectedTerminalType: "response.failed",
    },
  ] as const;

  for (const testCase of cases) {
    await t.step(testCase.name, async () => {
      const logs: unknown[][] = [];
      const originalInfo = console.info;
      console.info = (...args: unknown[]) => logs.push(args);
      try {
        const response = await withFetchMock(
          () =>
            sseResponse([
              "data: " + JSON.stringify({
                type: "response.created",
                response: { id: "resp_real_failure" },
              }) + "\n\n",
              "data: " + JSON.stringify(testCase.terminal) + "\n\n",
            ]),
          () => handleResponses(responsesRequest({ stream: true })),
        );
        const loggedResponse = await withTerminalRequestLog(response, {
          route: "responses",
          telemetryResponse: response,
          startedAtMonotonicMs: performance.now(),
          requestId: "real-responses-failure-" + testCase.name,
        });
        const serialized = await loggedResponse.text();
        assert.equal(response.status, 200);
        assert.deepEqual(
          parseResponsesSseValues(serialized).map((value) => value.type),
          ["response.created", testCase.terminal.type],
        );

        const telemetry = getResponseTelemetry(response);
        assert.ok(telemetry);
        assert.equal(telemetry.completed, false);
        assert.equal(telemetry.streamTerminalType, testCase.expectedTerminalType);
        assert.equal(telemetry.failureKind, testCase.expectedFailureKind);
        assert.equal(telemetry.syntheticTerminalType, null);
        assert.equal(telemetry.responseCreatedObserved, true);
        assert.ok(telemetry.failureKind !== null && telemetry.failureKind.length <= 128);
        const serializedTelemetry = JSON.stringify(telemetry);
        assert.equal(serializedTelemetry.includes(secretMessage), false);
        assert.equal(serializedTelemetry.includes(secretProviderField), false);

        const terminalLogs = logs
          .filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal")
          .map((entry) => JSON.parse(String(entry[1])) as Record<string, unknown>);
        assert.equal(terminalLogs.length, 1);
        const terminalLog = terminalLogs[0]!;
        assert.equal(terminalLog.stream_terminal_type, testCase.expectedTerminalType);
        assert.equal(terminalLog.failure_kind, testCase.expectedFailureKind);
        const serializedLog = JSON.stringify(terminalLog);
        assert.equal(serializedLog.includes(secretMessage), false);
        assert.equal(serializedLog.includes(secretProviderField), false);
      } finally {
        console.info = originalInfo;
      }
    });
  }
});

Deno.test("openai: streamed Responses force the SSE content type", async () => {
  const response = await withFetchMock(
    () =>
      new Response(sseResponse(baseSseChunks()).body, {
        status: 200,
        headers: {
          "Content-Encoding": "gzip",
          "Content-Length": "12345",
        },
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
  assert.equal(response.headers.get("Content-Encoding"), null);
  assert.equal(response.headers.get("Content-Length"), null);
  assert.match(await response.text(), /response.completed/);
});

Deno.test("openai: invalid route-dependent Responses fields fail before dispatch", async (t) => {
  const cases = [
    { param: "max_output_tokens", value: 0 },
    { param: "max_output_tokens", value: 1.5 },
    { param: "max_output_tokens", value: "12" },
    { param: "parallel_tool_calls", value: null },
    { param: "parallel_tool_calls", value: "true" },
    { param: "parallel_tool_calls", value: 1 },
  ] as const;
  for (const scenario of cases) {
    await t.step(`${scenario.param}=${String(scenario.value)}`, async () => {
      let fetches = 0;
      const response = await withFetchMock(
        () => {
          fetches += 1;
          return sseResponse(baseSseChunks());
        },
        () => handleResponses(responsesRequest({ [scenario.param]: scenario.value })),
      );
      assert.equal(response.status, 400);
      assert.equal((await response.json() as { error?: { param?: unknown } }).error?.param, scenario.param);
      assert.equal(fetches, 0);
    });
  }
});

Deno.test("openai: buffered Responses observe each real or synthetic terminal once", async (t) => {
  await t.step("real response.completed usage", async () => {
    const observations: Array<{ completed: boolean; totalTokens: number | null }> = [];
    const response = await withFetchMock(
      () => sseResponse(baseSseChunks()),
      () =>
        handleResponses(responsesRequest({ stream: false }), {
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          onTerminalUsage: (usage, completed) =>
            observations.push({
              completed,
              totalTokens: usage?.totalTokens ?? null,
            }),
        }),
    );
    await response.text();
    assert.deepEqual(observations, [{ completed: true, totalTokens: 2 }]);
  });
});

Deno.test("openai: buffered committed Responses failures use the official server_error code", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        ...baseSseChunks().slice(0, -1),
        'data: {"type":\n\n',
      ]),
    () => handleResponses(responsesRequest({ stream: false })),
  );
  assert.equal(response.status, 502);
  const payload = await response.json() as { error?: { code?: string } };
  assert.equal(payload.error?.code, "server_error");
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
  setKvForTest(null);
});
