import assert from "node:assert/strict";
import type { CodexBankedResetConfig } from "../src/codex_banked_reset.ts";
import type { CodexUsageResetProvider } from "../src/codex_banked_reset_provider.ts";
import type { ApiKeyHashRecord, ApiKeyUsageRequestV3, CodexAuthPoolState } from "../src/types.ts";
import { DEFAULT_MODEL_KEY, DEFAULT_REASONING_EFFORT_KEY } from "../src/defaults.ts";
import { setStreamFirstEventDeadlineMsForTest } from "../src/inference_deadline.ts";
import { RELEASE_GIT_SHA } from "../src/release.ts";
import { sha256Base64Url, sha256Hex } from "../src/utils.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
const TERRA_TEST_MODEL = "gpt-5.6-terra";
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
const { setOpenRouterApiKeyForTest } = await import("../src/openrouter.ts");
const {
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");
const { attemptCodexBankedReset } = await import("../src/codex_banked_reset.ts");
const {
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  getCodexQuotaBlockFence,
  isCodexQuotaBlockFenceCurrent,
  markCodexQuotaBlocked,
  markCodexUpstreamTimeout,
  selectCodexRoutingAccounts,
} = await import("../src/codex_account_routing.ts");
const { projectCerebrasToolSchema, setCerebrasFetchTimeoutMsForTest } = await import("../src/cerebras.ts");
const { OPENROUTER_CIRCUIT_KEY, parseOpenRouterCircuitState } = await import("../src/openrouter_circuit.ts");
const {
  ApiKeyQuotaDispatchError,
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV3RequestKey,
  apiKeyUsageV3WindowKey,
  reserveApiKeyUsageV3,
} = await import("../src/api_key_policy.ts");

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

const openRouterTextSseChunks = (
  options: Readonly<{
    model?: string | null;
    responseId?: string;
    text?: string;
    terminal?: boolean;
  }> = {},
): string[] => {
  const model = options.model === undefined ? "google/gemini-2.5-pro" : options.model;
  const responseId = options.responseId ?? "resp_openrouter_fixture";
  const text = options.text ?? "pong";
  const messageId = "msg_openrouter_fixture";
  const response = {
    id: responseId,
    object: "response",
    status: "in_progress",
    ...(model === null ? {} : { model }),
    output: [],
  };
  const chunks = [
    `data: ${JSON.stringify({ type: "response.created", sequence_number: 0, response })}\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.output_item.added",
        sequence_number: 1,
        response_id: responseId,
        output_index: 0,
        item: { id: messageId, type: "message", status: "in_progress", role: "assistant", content: [] },
      })
    }\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.output_text.delta",
        sequence_number: 2,
        response_id: responseId,
        item_id: messageId,
        output_index: 0,
        content_index: 0,
        delta: text,
      })
    }\n\n`,
    `data: ${
      JSON.stringify({
        type: "response.output_item.done",
        sequence_number: 3,
        response_id: responseId,
        output_index: 0,
        item: {
          id: messageId,
          type: "message",
          status: "completed",
          role: "assistant",
          content: [{ type: "output_text", text, annotations: [] }],
        },
      })
    }\n\n`,
  ];
  if (options.terminal === false) return chunks;
  chunks.push(`data: ${
    JSON.stringify({
      type: "response.completed",
      sequence_number: 4,
      response: {
        ...response,
        status: "completed",
        output: [{
          id: messageId,
          type: "message",
          status: "completed",
          role: "assistant",
          content: [{ type: "output_text", text, annotations: [] }],
        }],
        usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
      },
    })
  }\n\n`);
  return chunks;
};

const openRouterResponsesRequest = (
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

const openRouterCircuitState = (): ReturnType<typeof parseOpenRouterCircuitState> =>
  parseOpenRouterCircuitState(kvStore.get(keyToString(OPENROUTER_CIRCUIT_KEY)));

const waitForOpenRouterCircuit = async (
  predicate: (state: NonNullable<ReturnType<typeof parseOpenRouterCircuitState>>) => boolean,
): Promise<NonNullable<ReturnType<typeof parseOpenRouterCircuitState>>> => {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const state = openRouterCircuitState();
    if (state && predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  const state = openRouterCircuitState();
  assert.fail(`OpenRouter circuit did not reach the expected state: ${JSON.stringify(state)}`);
};

const seedOpenRouterCircuit = (
  state: Readonly<{
    phase: "open" | "half_open";
    openUntilMs: number;
    generation?: number;
    probe?:
      | Readonly<{
        token: string;
        generation: number;
        lease_until_ms: number;
        source: "expiry" | "early_recovery";
      }>
      | null;
  }>,
): void => {
  kvStore.set(keyToString(OPENROUTER_CIRCUIT_KEY), {
    v: 1,
    phase: state.phase,
    failure_at_ms: [Date.now() - 1_000, Date.now() - 500],
    open_until_ms: state.openUntilMs,
    generation: state.generation ?? 1,
    probe: state.probe ?? null,
    updated_at_ms: Date.now(),
  });
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
  options: Readonly<{ openRouterApiKey?: string }> = {},
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
  kvStore.delete(keyToString(["uos_ai", "openrouter_failover", "circuit", "v1"]));
  kvStore.delete(keyToString(["uos_ai", "openrouter_failover", "telemetry", "v1"]));
  resetCodexAuthCacheForTest();

  setOpenRouterApiKeyForTest(options.openRouterApiKey ?? null);

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
    setOpenRouterApiKeyForTest(undefined);
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
            const responseStatus = response.status;
            const responseContentType = response.headers.get("Content-Type");
            const responseUpstream = response.headers.get("x-uos-upstream");
            // A streamed response owns the recovery probe until its terminal
            // event is consumed and validated; merely creating the Response
            // is not proof of successful recovery.
            const responseBody = await response.text();
            const authPool = kvStore.get(keyToString(["ubq_ai", "codex_auth"])) as CodexAuthPoolState;
            const routingAfterRecovery = await selectCodexRoutingAccounts(authPool, authPool.accounts, Date.now());
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

Deno.test("openai: timeout-circuit short circuits remain gateway-generated", async () => {
  const response = await withFetchMock(
    () => {
      throw new Error("a timeout circuit response must not dispatch to Codex");
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

  assert.equal(response.status, 503);
  assert.equal(response.headers.get("x-uos-upstream"), null);
  assert.deepEqual(await response.json(), {
    error: {
      message: "Codex upstream is temporarily unavailable after response-header timeouts; retry later.",
      type: "server_error",
      code: "codex_upstream_degraded",
      param: null,
    },
  });
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

Deno.test("openai: Terra Chat Completions maps the completion cap and explicitly ignores temperature", async () => {
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
  assert.equal(warnings.includes("max_output_tokens_ignored"), false);
  assert.ok(recordedBody);
  const recorded = recordedBody as Record<string, unknown>;
  assert.equal(recorded.model, TERRA_TEST_MODEL);
  assert.equal(recorded.max_output_tokens, 2048);
  assert.equal("max_completion_tokens" in recorded, false);
  assert.equal("temperature" in recorded, false);
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
        supported_reasoning_levels: ["medium"],
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
      () => handleResponses(openRouterResponsesRequest()),
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

Deno.test("openai: an all-blocked Codex response continues through paid YunWu fallback", async () => {
  const authKey = keyToString(["ubq_ai", "codex_auth"]);
  const previousAuth = kvStore.get(authKey);
  const previousYunwuKey = Deno.env.get("YUNWU_API_KEY");
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
  let yunwuCalls = 0;
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  seedPaidFallbackKey(keyId);

  try {
    await withFetchMock(
      (url) => {
        if (url === "https://yunwu.ai/v1/responses") {
          yunwuCalls += 1;
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
        assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
        assert.equal(yunwuCalls, 1);
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
    if (previousYunwuKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", previousYunwuKey);
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
      providerRequestId: null,
      firstProviderDispatchMs: null,
      firstProviderHeadersMs: null,
      firstCodexDispatchMs: null,
      firstCodexHeadersMs: null,
      firstSseEventMs: null,
      streamTerminalMs: null,
      attemptedProviders: ["chatgpt_codex"],
      openRouterTriggerClass: null,
      openRouterCircuitTransition: null,
      openRouterSelectedModel: null,
      openRouterTaskType: null,
      openRouterSemanticCommitment: null,
      openRouterLatencyMs: null,
      openRouterTerminalStatus: null,
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
        assert.equal(typeof telemetry?.firstProviderDispatchMs, "number");
        assert.equal(typeof telemetry?.firstProviderHeadersMs, "number");
        const logText = JSON.stringify(logs);
        assert.doesNotMatch(logText, /cerebras-test-key/);
        assert.doesNotMatch(logText, /provider-body-must-not-be-logged-or-relayed/);
      } finally {
        console.error = originalError;
      }
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
          ),
      );
      assert.equal(streamResponse.status, 200);
      assert.equal(streamResponse.headers.get("Content-Type"), "text/event-stream");
      assert.equal(streamResponse.headers.get("x-uos-warning"), "gpt_oss_stream_downgraded");
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
            () => handleChatCompletions(request(canonicalBody)),
          );
          assert.equal(response.status, testCase.status);
          assert.equal(response.headers.get("x-uos-upstream"), "cerebras");
          assert.equal(response.headers.get("x-uos-provider-request-id"), `cerebras-error-${testCase.status}`);
          assert.equal(response.headers.get("Retry-After"), testCase.status === 429 ? "17" : null);
          for (const [header, value] of Object.entries(cerebrasRateLimitHeaders)) {
            assert.equal(
              response.headers.get(header),
              testCase.status === 429 ? value : null,
              `${header} on ${testCase.status}`,
            );
          }
          const payload = await response.json() as { error?: { message?: string; type?: string; code?: string } };
          assert.equal(payload.error?.type, testCase.expectedType);
          assert.equal(payload.error?.code, "cerebras_upstream_error");
          assert.doesNotMatch(payload.error?.message ?? "", /provider-body-must-not-be-logged-or-relayed/);
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
      }
    });

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
      assert.equal(upstreamCancelled, true);
    });
  } finally {
    restoreApiKey();
    setCerebrasFetchTimeoutMsForTest(null);
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

Deno.test("openai: eligible Responses failure replays through OpenRouter Auto", async () => {
  const primaryBody = JSON.stringify({
    model: DEFAULT_TEST_MODEL,
    input: "ping",
    stream: true,
    reasoning: { effort: "ultra" },
    max_output_tokens: 256,
    client_metadata: { session_id: "raw-session-id" },
    tools: [
      {
        type: "function",
        name: "lookup",
        description: "Look up a value",
        parameters: { type: "object", properties: {}, additionalProperties: false },
      },
      { type: "custom", name: "exec", description: "Run a command", format: { type: "text" } },
    ],
    tool_choice: "auto",
    parallel_tool_calls: true,
  });
  const urls: string[] = [];
  let openRouterBody: Record<string, unknown> | null = null;
  let openRouterAuthorization: string | null = null;
  let openRouterMetadata: string | null = null;
  const response = await withFetchMock(
    (url, bodyText, init) => {
      urls.push(url);
      if (url !== "https://openrouter.ai/api/v1/responses") {
        return new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        });
      }
      openRouterBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      const headers = new Headers(init?.headers);
      openRouterAuthorization = headers.get("Authorization");
      openRouterMetadata = headers.get("X-OpenRouter-Metadata");
      return sseResponse(openRouterTextSseChunks());
    },
    () =>
      handleResponses(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: primaryBody,
        }),
        {
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          idempotencyPrincipal: "api-key:test-principal",
        },
      ),
    { openRouterApiKey: "or-test-key" },
  );

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
  assert.deepEqual(urls, [
    "https://chatgpt.com/backend-api/codex/responses",
    "https://openrouter.ai/api/v1/responses",
  ]);
  assert.ok(openRouterBody);
  const sent = openRouterBody as Record<string, unknown>;
  assert.equal(sent.model, "openrouter/auto");
  assert.deepEqual(sent.plugins, [{
    id: "auto-router",
    cost_tier: "max",
    excluded_models: [
      "openai/*",
      "~openai/*",
      "anthropic/*",
      "~anthropic/*",
      "*/gpt-*",
      "*/claude-*",
    ],
  }]);
  assert.deepEqual(sent.reasoning, { effort: "max" });
  assert.equal(sent.max_output_tokens, 256);
  assert.equal(typeof sent.session_id, "string");
  assert.doesNotMatch(String(sent.session_id), /raw-session-id|test-principal/);
  assert.equal(JSON.stringify(sent).includes("raw-session-id"), false);
  assert.equal(openRouterAuthorization, "Bearer or-test-key");
  assert.equal(openRouterMetadata, "enabled");

  const text = await response.text();
  const events = [...text.matchAll(/^data: (.+)$/gm)]
    .map((match) => JSON.parse(match[1]!) as Record<string, unknown>);
  const warning = events.find((event) =>
    event.type === "response.output_text.delta" &&
    String(event.delta).includes("Failover active")
  );
  assert.equal(
    warning?.delta,
    "⚠ Failover active: this response is from `openrouter:google/gemini-2.5-pro` because the Codex upstream was unavailable.",
  );
  const providerDelta = events.find((event) => event.delta === "pong");
  assert.equal(providerDelta?.output_index, 1);
  assert.deepEqual(events.map((event) => event.sequence_number), events.map((_, index) => index));
  assert.equal(events.filter((event) => event.type === "response.completed").length, 1);
  assert.equal(events.filter((event) => event.type === "response.failed").length, 0);
  assert.equal(
    parseWarnings(response.headers.get("x-uos-warning")).includes("max_output_tokens_ignored"),
    false,
  );
  const terminal = events.find((event) => event.type === "response.completed");
  const terminalOutput = (terminal?.response as { output?: unknown[] } | undefined)?.output ?? [];
  assert.equal(terminalOutput.length, 2);
  assert.equal((terminalOutput[0] as { role?: unknown } | undefined)?.role, "assistant");
  assert.equal(getResponseTelemetry(response)?.provider, "openrouter");
  assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["chatgpt_codex", "openrouter"]);
});

Deno.test("openai: OpenRouter handler failover covers precommit failures and commitment barriers", async (t) => {
  const scenarios = [
    {
      name: "missing body",
      primary: () => new Response(null, { status: 200, headers: { "Content-Type": "text/event-stream" } }),
      trigger: "missing_body",
    },
    {
      name: "malformed SSE",
      primary: () => sseResponse(['data: {"type":\n\n']),
      trigger: "malformed_event",
    },
    {
      name: "premature EOF",
      primary: () =>
        sseResponse([
          `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_primary_setup" } })}\n\n`,
        ]),
      trigger: "premature_eof",
    },
    {
      name: "response.failed terminal",
      primary: () =>
        sseResponse([
          `data: ${
            JSON.stringify({
              type: "response.failed",
              response: {
                id: "resp_primary_failed",
                object: "response",
                status: "failed",
                error: { type: "server_error", code: "provider_error", message: "primary failed" },
                output: [],
              },
            })
          }\n\n`,
        ]),
      trigger: "terminal_failure",
    },
    {
      name: "error terminal",
      primary: () =>
        sseResponse([
          `data: ${
            JSON.stringify({
              type: "error",
              error: { type: "server_error", code: "provider_error", message: "primary failed" },
            })
          }\n\n`,
        ]),
      trigger: "terminal_failure",
    },
  ] as const;
  for (const scenario of scenarios) {
    await t.step(scenario.name, async () => {
      const urls: string[] = [];
      const response = await withFetchMock(
        (url) => {
          urls.push(url);
          return url === "https://openrouter.ai/api/v1/responses"
            ? sseResponse(openRouterTextSseChunks())
            : scenario.primary();
        },
        () => handleResponses(openRouterResponsesRequest()),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 200);
      assert.deepEqual(urls, [
        "https://chatgpt.com/backend-api/codex/responses",
        "https://openrouter.ai/api/v1/responses",
      ]);
      assert.equal(getResponseTelemetry(response)?.openRouterTriggerClass, scenario.trigger);
      assert.match(await response.text(), /Failover active/);
    });
  }

  await t.step("semantic timeout", async () => {
    setStreamFirstEventDeadlineMsForTest(10);
    try {
      let openRouterCalls = 0;
      let primaryCancelled = false;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://openrouter.ai/api/v1/responses") {
            openRouterCalls += 1;
            return sseResponse(openRouterTextSseChunks());
          }
          return new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                controller.enqueue(TEXT_ENCODER.encode(
                  `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_setup" } })}\n\n`,
                ));
              },
              cancel() {
                primaryCancelled = true;
              },
            }),
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          );
        },
        () => handleResponses(openRouterResponsesRequest()),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 200);
      assert.equal(openRouterCalls, 1);
      assert.equal(primaryCancelled, true);
      assert.equal(getResponseTelemetry(response)?.openRouterTriggerClass, "semantic_timeout");
      assert.match(await response.text(), /Failover active/);
    } finally {
      setStreamFirstEventDeadlineMsForTest(null);
    }
  });

  const committed = [
    {
      name: "text",
      event: {
        type: "response.output_text.delta",
        response_id: "resp_primary",
        item_id: "msg_primary",
        output_index: 0,
        content_index: 0,
        delta: "primary text",
      },
    },
    {
      name: "reasoning",
      event: {
        type: "response.reasoning_summary_text.delta",
        response_id: "resp_primary",
        item_id: "rs_primary",
        output_index: 0,
        summary_index: 0,
        delta: "primary reasoning",
      },
    },
    {
      name: "function call",
      event: {
        type: "response.output_item.done",
        response_id: "resp_primary",
        output_index: 0,
        item: {
          id: "fc_primary",
          type: "function_call",
          status: "completed",
          call_id: "call_primary",
          name: "lookup",
          arguments: "{}",
        },
      },
    },
    {
      name: "custom tool",
      event: {
        type: "response.output_item.done",
        response_id: "resp_primary",
        output_index: 0,
        item: {
          id: "ctc_primary",
          type: "custom_tool_call",
          status: "completed",
          call_id: "call_custom_primary",
          name: "exec",
          input: "pwd",
        },
      },
    },
  ] as const;
  for (const scenario of committed) {
    await t.step(`no replay after ${scenario.name}`, async () => {
      let openRouterCalls = 0;
      const response = await withFetchMock(
        (url) => {
          if (url === "https://openrouter.ai/api/v1/responses") {
            openRouterCalls += 1;
            return sseResponse(openRouterTextSseChunks());
          }
          return sseResponse([
            `data: ${
              JSON.stringify({
                type: "response.created",
                response: { id: "resp_primary", object: "response", status: "in_progress", output: [] },
              })
            }\n\n`,
            `data: ${JSON.stringify(scenario.event)}\n\n`,
          ]);
        },
        () => handleResponses(openRouterResponsesRequest()),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 200);
      const values = parseResponsesSseValues(await response.text());
      assert.equal(openRouterCalls, 0);
      assert.equal(values.filter((event) => event.type === "response.failed").length, 1);
      assert.equal(values.some((event) => JSON.stringify(event).includes("Failover active")), false);
    });
  }

  await t.step("missing response template uses an official error event", async () => {
    let openRouterCalls = 0;
    const response = await withFetchMock(
      (url) => {
        if (url === "https://openrouter.ai/api/v1/responses") {
          openRouterCalls += 1;
          return sseResponse(openRouterTextSseChunks());
        }
        return sseResponse([
          `data: ${
            JSON.stringify({
              type: "response.output_text.delta",
              response_id: "resp_primary_without_created",
              item_id: "msg_primary_without_created",
              output_index: 0,
              content_index: 0,
              delta: "primary text",
            })
          }\n\n`,
        ]);
      },
      () => handleResponses(openRouterResponsesRequest()),
      { openRouterApiKey: "or-test-key" },
    );
    const values = parseResponsesSseValues(await response.text());
    assert.equal(openRouterCalls, 0);
    assert.equal(values.filter((event) => event.type === "response.failed").length, 0);
    const error = values.find((event) => event.type === "error");
    assert.equal(error?.code, "server_error");
    assert.equal(error?.param, null);
    assert.equal(Object.prototype.hasOwnProperty.call(error ?? {}, "response"), false);
  });
});

Deno.test("openai: OpenRouter pre-output rejection restores the authoritative primary error", async (t) => {
  for (
    const model of [
      null,
      "openrouter/auto",
      "openai/gpt-5",
      "anthropic/claude-sonnet-4",
      "vendor/gpt-oss-120b",
      "malformed",
    ]
  ) {
    await t.step(String(model), async () => {
      const response = await withFetchMock(
        (url) =>
          url === "https://openrouter.ai/api/v1/responses"
            ? sseResponse(openRouterTextSseChunks({ model }))
            : new Response(JSON.stringify({ error: { message: "Primary unavailable", code: "primary_fixture" } }), {
              status: 503,
              headers: { "Content-Type": "application/json", "Retry-After": "17" },
            }),
        () => handleResponses(openRouterResponsesRequest()),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 503);
      assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
      assert.equal(response.headers.get("Retry-After"), "17");
      const payload = await response.json() as { error?: { message?: string; code?: string } };
      assert.equal(payload.error?.message, "Primary unavailable");
      assert.equal(payload.error?.code, "primary_fixture");
    });
  }

  await t.step("fallback 5xx", async () => {
    const response = await withFetchMock(
      (url) =>
        url === "https://openrouter.ai/api/v1/responses"
          ? new Response(JSON.stringify({ error: { message: "fallback failed" } }), {
            status: 502,
            headers: { "Content-Type": "application/json" },
          })
          : new Response(JSON.stringify({ error: { message: "Primary unavailable", code: "primary_fixture" } }), {
            status: 503,
            headers: { "Content-Type": "application/json" },
          }),
      () => handleResponses(openRouterResponsesRequest()),
      { openRouterApiKey: "or-test-key" },
    );
    assert.equal(response.status, 503);
    assert.equal((await response.json() as { error?: { code?: string } }).error?.code, "primary_fixture");
    assert.equal(getResponseTelemetry(response)?.provider, "chatgpt_codex");
    const persisted = kvStore.get(keyToString(["uos_ai", "openrouter_failover", "telemetry", "v1"])) as
      | Record<string, unknown>
      | undefined;
    assert.equal(persisted?.attempted_provider, "chatgpt_codex,openrouter");
    assert.equal(persisted?.terminal_status, "failed_before_commit");
    assert.equal(persisted?.trigger_class, "http_5xx");
  });
});

Deno.test("openai: OpenRouter post-release failures own one synthetic terminal", async (t) => {
  for (const scenario of ["eof", "malformed"] as const) {
    await t.step(scenario, async () => {
      const response = await withFetchMock(
        (url) => {
          if (url !== "https://openrouter.ai/api/v1/responses") {
            return new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
              status: 503,
              headers: { "Content-Type": "application/json" },
            });
          }
          const chunks = openRouterTextSseChunks({ terminal: false });
          if (scenario === "malformed") chunks.push('data: {"type":\n\n');
          return sseResponse(chunks);
        },
        () => handleResponses(openRouterResponsesRequest()),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 200);
      const values = parseResponsesSseValues(await response.text());
      assert.equal(values.filter((event) => event.type === "response.failed").length, 1);
      assert.equal(values.filter((event) => event.type === "response.completed").length, 0);
      assert.equal(values.filter((event) => event.type === "error").length, 0);
      assert.equal(values.some((event) => JSON.stringify(event).includes("Failover active")), true);
      const terminal = values.find((event) => event.type === "response.failed")!;
      const terminalResponse = terminal.response as Record<string, unknown>;
      assert.equal(terminalResponse.model, "google/gemini-2.5-pro");
      assert.match(JSON.stringify(terminalResponse.output), /pong/);
      assert.deepEqual(
        values.map((event) => event.sequence_number),
        values.map((_, index) => index),
      );
      assert.equal(getResponseTelemetry(response)?.streamTerminalType, "response.failed");
      assert.equal(getResponseTelemetry(response)?.openRouterTerminalStatus, "response.failed");
      assert.equal(getResponseTelemetry(response)?.completed, false);
    });
  }
});

Deno.test("openai: buffered fallback keeps provider deltas when terminal output is empty", async () => {
  const chunks = openRouterTextSseChunks();
  const terminal = JSON.parse(chunks.at(-1)!.match(/^data: (.+)\n\n$/)![1]!) as Record<string, unknown>;
  (terminal.response as Record<string, unknown>).output = [];
  chunks[chunks.length - 1] = `data: ${JSON.stringify(terminal)}\n\n`;
  const response = await withFetchMock(
    (url) =>
      url === "https://openrouter.ai/api/v1/responses"
        ? sseResponse(chunks)
        : new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        }),
    () => handleResponses(openRouterResponsesRequest({ stream: false })),
    { openRouterApiKey: "or-test-key" },
  );
  assert.equal(response.status, 200);
  const payload = await response.json() as Record<string, unknown>;
  const output = payload.output as Array<Record<string, unknown>>;
  assert.equal(output.length, 2);
  assert.match(JSON.stringify(output[0]), /Failover active/);
  assert.match(JSON.stringify(output[1]), /pong/);
});

Deno.test("openai: OpenRouter preserves a first-semantic incomplete terminal", async () => {
  const responseId = "resp_openrouter_incomplete";
  const response = await withFetchMock(
    (url) => {
      if (url !== "https://openrouter.ai/api/v1/responses") {
        return new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        });
      }
      return sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.created",
            sequence_number: 0,
            response: {
              id: responseId,
              object: "response",
              status: "in_progress",
              model: "google/gemini-2.5-pro",
              output: [],
            },
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.incomplete",
            sequence_number: 1,
            response: {
              id: responseId,
              object: "response",
              status: "incomplete",
              model: "google/gemini-2.5-pro",
              incomplete_details: { reason: "max_output_tokens" },
              output: [{
                id: "msg_openrouter_incomplete",
                type: "message",
                status: "incomplete",
                role: "assistant",
                content: [{ type: "output_text", text: "partial", annotations: [] }],
              }],
              usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
            },
          })
        }\n\n`,
      ]);
    },
    () => handleResponses(openRouterResponsesRequest()),
    { openRouterApiKey: "or-test-key" },
  );
  assert.equal(response.status, 200);
  const values = parseResponsesSseValues(await response.text());
  const terminal = values.find((event) => event.type === "response.incomplete");
  assert.ok(terminal);
  assert.match(JSON.stringify(terminal), /partial/);
  assert.match(JSON.stringify(terminal), /max_output_tokens/);
  assert.equal(values.filter((event) => event.type === "response.failed").length, 0);
});

Deno.test("openai: successful OpenRouter failover preserves primary remediation warnings", async (t) => {
  for (const stream of [false, true]) {
    await t.step(stream ? "streamed" : "buffered", async () => {
      const response = await withFetchMock(
        (url) =>
          url === "https://openrouter.ai/api/v1/responses"
            ? sseResponse(openRouterTextSseChunks())
            : new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
              status: 503,
              headers: {
                "Content-Type": "application/json",
                "x-uos-warning": CODEX_AUTH_REAUTH_WARNING,
              },
            }),
        () => handleResponses(openRouterResponsesRequest({ stream })),
        { openRouterApiKey: "or-test-key" },
      );
      assert.equal(response.status, 200);
      assert.ok(parseWarnings(response.headers.get("x-uos-warning")).includes(CODEX_AUTH_REAUTH_WARNING));
      await response.body?.cancel();
    });
  }
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
  for (const circuit of ["closed", "open"] as const) {
    for (const scenario of cases) {
      await t.step(`${circuit} ${scenario.param}=${String(scenario.value)}`, async () => {
        let fetches = 0;
        const response = await withFetchMock(
          () => {
            fetches += 1;
            return sseResponse(baseSseChunks());
          },
          async () => {
            if (circuit === "open") seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
            return await handleResponses(openRouterResponsesRequest({ [scenario.param]: scenario.value }));
          },
          { openRouterApiKey: "or-test-key" },
        );
        assert.equal(response.status, 400);
        assert.equal((await response.json() as { error?: { param?: unknown } }).error?.param, scenario.param);
        assert.equal(fetches, 0);
      });
    }
  }
});

Deno.test("openai: OpenRouter quota dispatch errors propagate for outer status conversion", async () => {
  let fetches = 0;
  await assert.rejects(
    () =>
      withFetchMock(
        () => {
          fetches += 1;
          return sseResponse(openRouterTextSseChunks());
        },
        async () => {
          seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
          return await handleResponses(openRouterResponsesRequest(), {
            keyId: "quota-fixture",
            kernelRepo: null,
            kernelOrg: null,
            beforeProviderDispatch: () =>
              Promise.reject(
                new ApiKeyQuotaDispatchError("Fixture quota exhausted", {
                  status: 429,
                  code: "rate_limit_exceeded",
                  errorType: "rate_limit_error",
                  retryAfter: "17",
                }),
              ),
          });
        },
        { openRouterApiKey: "or-test-key" },
      ),
    (error: unknown) => {
      assert.ok(error instanceof ApiKeyQuotaDispatchError);
      assert.equal(error.status, 429);
      assert.equal(error.code, "rate_limit_exceeded");
      assert.equal(error.errorType, "rate_limit_error");
      assert.equal(error.retryAfter, "17");
      return true;
    },
  );
  assert.equal(fetches, 0);
});

Deno.test("openai: OpenRouter keeps native custom-tool events and identities", async () => {
  const responseId = "resp_openrouter_custom";
  const itemId = "ctc_openrouter_custom";
  const callId = "call_openrouter_custom";
  let fallbackBody: Record<string, unknown> | null = null;
  const response = await withFetchMock(
    (url, bodyText) => {
      if (url !== "https://openrouter.ai/api/v1/responses") {
        return new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        });
      }
      fallbackBody = bodyText ? JSON.parse(bodyText) as Record<string, unknown> : null;
      const item = {
        id: itemId,
        type: "custom_tool_call",
        status: "completed",
        call_id: callId,
        name: "exec",
        input: "pwd",
      };
      return sseResponse([
        `data: ${
          JSON.stringify({
            type: "response.created",
            sequence_number: 0,
            response: {
              id: responseId,
              object: "response",
              status: "in_progress",
              model: "google/gemini-2.5-pro",
              output: [],
            },
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.output_item.added",
            sequence_number: 1,
            response_id: responseId,
            output_index: 0,
            item: { ...item, status: "in_progress", input: "" },
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.custom_tool_call_input.delta",
            sequence_number: 2,
            response_id: responseId,
            item_id: itemId,
            output_index: 0,
            delta: "pwd",
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.custom_tool_call_input.done",
            sequence_number: 3,
            response_id: responseId,
            item_id: itemId,
            output_index: 0,
            input: "pwd",
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.output_item.done",
            sequence_number: 4,
            response_id: responseId,
            output_index: 0,
            item,
          })
        }\n\n`,
        `data: ${
          JSON.stringify({
            type: "response.completed",
            sequence_number: 5,
            response: {
              id: responseId,
              object: "response",
              status: "completed",
              model: "google/gemini-2.5-pro",
              output: [item],
              usage: { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
            },
          })
        }\n\n`,
      ]);
    },
    () =>
      handleResponses(openRouterResponsesRequest({
        tools: [{ type: "custom", name: "exec", description: "Run a command", format: { type: "text" } }],
        input: [{ type: "custom_tool_call_output", call_id: "prior_call", output: "prior result" }],
      })),
    { openRouterApiKey: "or-test-key" },
  );

  assert.equal(response.status, 200);
  const sentFallbackBody = fallbackBody as unknown as Record<string, unknown>;
  assert.deepEqual(sentFallbackBody.tools, [{
    type: "custom",
    name: "exec",
    description: "Run a command",
    format: { type: "text" },
  }]);
  assert.deepEqual(sentFallbackBody.input, [{
    type: "custom_tool_call_output",
    call_id: "prior_call",
    output: "prior result",
  }]);
  const values = parseResponsesSseValues(await response.text());
  const delta = values.find((event) => event.type === "response.custom_tool_call_input.delta");
  const done = values.find((event) => event.type === "response.custom_tool_call_input.done");
  const itemDone = values.find((event) =>
    event.type === "response.output_item.done" &&
    (event.item as { id?: unknown } | undefined)?.id === itemId
  );
  assert.equal(delta?.item_id, itemId);
  assert.equal(delta?.output_index, 1);
  assert.equal(done?.item_id, itemId);
  assert.equal(done?.input, "pwd");
  assert.equal((itemDone?.item as { call_id?: unknown } | undefined)?.call_id, callId);
  assert.equal((itemDone?.item as { name?: unknown } | undefined)?.name, "exec");
  assert.equal((itemDone?.item as { input?: unknown } | undefined)?.input, "pwd");
  assert.equal(itemDone?.output_index, 1);
  const terminal = values.find((event) => event.type === "response.completed");
  const output = (terminal?.response as { output?: unknown[] } | undefined)?.output ?? [];
  assert.equal(output.length, 2);
  assert.equal((output[1] as { id?: unknown } | undefined)?.id, itemId);
  assert.equal((output[1] as { call_id?: unknown } | undefined)?.call_id, callId);
});

Deno.test("openai: Codex and YunWu remain ahead of one OpenRouter rescue", async () => {
  const keyId = "openrouter-after-yunwu";
  const requestId = "request-openrouter-after-yunwu";
  const previousYunwuKey = Deno.env.get("YUNWU_API_KEY");
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  seedPaidFallbackKey(keyId);
  const urls: string[] = [];
  try {
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        if (url === "https://openrouter.ai/api/v1/responses") {
          return sseResponse(openRouterTextSseChunks());
        }
        if (url === "https://yunwu.ai/v1/responses") {
          return new Response(JSON.stringify({ error: { message: "YunWu unavailable" } }), {
            status: 503,
            headers: {
              "Content-Type": "application/json",
              "X-Oneapi-Request-Id": "yunwu-openrouter-order-fixture",
            },
          });
        }
        return new Response(JSON.stringify({ error: { message: "Codex limited" } }), {
          status: 429,
          headers: { "Content-Type": "application/json" },
        });
      },
      async () => {
        const response = await handleResponses(openRouterResponsesRequest(), {
          keyId,
          kernelRepo: null,
          kernelOrg: null,
          requestId,
          startedAtMs: Date.now(),
        });
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.deepEqual(urls, [
      "https://chatgpt.com/backend-api/codex/responses",
      "https://chatgpt.com/backend-api/codex/responses",
      "https://yunwu.ai/v1/responses",
      "https://openrouter.ai/api/v1/responses",
    ]);
    assert.equal(urls.filter((url) => url === "https://yunwu.ai/v1/responses").length, 1);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["chatgpt_codex", "yunwu", "openrouter"]);
    const stored = await waitForPaidFallbackTerminal(keyId, requestId, "failed");
    assert.equal(stored.dispatch_state, "dispatched");
    assert.equal(stored.billing_state, "pending");
    assert.equal(stored.provider_request_id, "yunwu-openrouter-order-fixture");
  } finally {
    if (previousYunwuKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", previousYunwuKey);
  }
});

Deno.test("openai: OpenRouter circuit routes and recovers at handler semantic boundaries", async (t) => {
  await t.step("active open circuit dispatches only OpenRouter", async () => {
    const urls: string[] = [];
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        assert.equal(url, "https://openrouter.ai/api/v1/responses");
        return sseResponse(openRouterTextSseChunks());
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.deepEqual(urls, ["https://openrouter.ai/api/v1/responses"]);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["openrouter"]);
    assert.equal(openRouterCircuitState()?.phase, "open");
  });

  await t.step("expired open circuit closes on the half-open Codex semantic event", async () => {
    const urls: string[] = [];
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        assert.equal(url, "https://chatgpt.com/backend-api/codex/responses");
        return sseResponse(baseSseChunks());
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "closed");
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.deepEqual(urls, ["https://chatgpt.com/backend-api/codex/responses"]);
    assert.equal(state.probe, null);
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "closed");
  });

  await t.step("semantic failed half-open Codex probe reopens the circuit", async () => {
    const response = await withFetchMock(
      (url) => {
        assert.equal(url, "https://chatgpt.com/backend-api/codex/responses");
        return sseResponse([
          `data: ${
            JSON.stringify({
              type: "response.created",
              response: { id: "resp_failed_probe", object: "response", status: "in_progress", output: [] },
            })
          }\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.failed",
              response: {
                id: "resp_failed_probe",
                object: "response",
                status: "failed",
                error: { code: "server_error", message: "Probe failed." },
                output: [{
                  id: "msg_failed_probe",
                  type: "message",
                  status: "incomplete",
                  role: "assistant",
                  content: [{ type: "output_text", text: "partial", annotations: [] }],
                }],
              },
            })
          }\n\n`,
        ]);
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "open" && candidate.probe === null);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.ok((state.open_until_ms ?? 0) > Date.now());
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "reopened");
  });

  await t.step("expired open circuit closes on an empty Codex completion", async () => {
    const response = await withFetchMock(
      (url) => {
        assert.equal(url, "https://chatgpt.com/backend-api/codex/responses");
        return sseResponse([
          `data: ${
            JSON.stringify({
              type: "response.created",
              response: {
                id: "resp_empty_probe",
                object: "response",
                created_at: 1,
                model: DEFAULT_TEST_MODEL,
                status: "in_progress",
                output: [],
              },
            })
          }\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.completed",
              response: {
                id: "resp_empty_probe",
                object: "response",
                created_at: 1,
                model: DEFAULT_TEST_MODEL,
                status: "completed",
                output: [],
                usage: { input_tokens: 1, output_tokens: 0, total_tokens: 1 },
              },
            })
          }\n\n`,
        ]);
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "closed");
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.equal(state.probe, null);
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "closed");
  });

  await t.step("expired open circuit closes on an empty Codex incomplete terminal", async () => {
    const response = await withFetchMock(
      (url) => {
        assert.equal(url, "https://chatgpt.com/backend-api/codex/responses");
        return sseResponse([
          `data: ${
            JSON.stringify({
              type: "response.created",
              response: {
                id: "resp_empty_incomplete_probe",
                object: "response",
                created_at: 1,
                model: DEFAULT_TEST_MODEL,
                status: "in_progress",
                output: [],
              },
            })
          }\n\n`,
          `data: ${
            JSON.stringify({
              type: "response.incomplete",
              response: {
                id: "resp_empty_incomplete_probe",
                object: "response",
                created_at: 1,
                model: DEFAULT_TEST_MODEL,
                status: "incomplete",
                incomplete_details: { reason: "max_output_tokens" },
                output: [],
                usage: { input_tokens: 1, output_tokens: 0, total_tokens: 1 },
              },
            })
          }\n\n`,
        ]);
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "closed");
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.equal(state.probe, null);
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "closed");
  });

  await t.step("half-open YunWu semantic output releases rather than closes the Codex circuit", async () => {
    const keyId = "openrouter-half-open-yunwu";
    const requestId = "request-openrouter-half-open-yunwu";
    const previousYunwuKey = Deno.env.get("YUNWU_API_KEY");
    Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
    seedPaidFallbackKey(keyId);
    const urls: string[] = [];
    try {
      const response = await withFetchMock(
        (url) => {
          urls.push(url);
          if (url === "https://yunwu.ai/v1/responses") return sseResponse(baseSseChunks());
          return new Response(JSON.stringify({ error: { message: "Codex limited" } }), {
            status: 429,
            headers: { "Content-Type": "application/json" },
          });
        },
        async () => {
          seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
          const response = await handleResponses(openRouterResponsesRequest(), {
            keyId,
            kernelRepo: null,
            kernelOrg: null,
            requestId,
            startedAtMs: Date.now(),
          });
          await response.text();
          return response;
        },
        { openRouterApiKey: "or-test-key" },
      );

      const state = await waitForOpenRouterCircuit((candidate) =>
        candidate.phase === "open" && candidate.probe === null
      );
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
      assert.equal(urls.includes("https://yunwu.ai/v1/responses"), true);
      assert.equal(urls.includes("https://openrouter.ai/api/v1/responses"), false);
      assert.ok((state.open_until_ms ?? Number.MAX_SAFE_INTEGER) <= Date.now());
      assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "released");
    } finally {
      if (previousYunwuKey === undefined) Deno.env.delete("YUNWU_API_KEY");
      else Deno.env.set("YUNWU_API_KEY", previousYunwuKey);
    }
  });

  await t.step("direct OpenRouter failure claims one successful early Codex recovery", async () => {
    const urls: string[] = [];
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        if (url === "https://openrouter.ai/api/v1/responses") {
          return new Response(JSON.stringify({ error: { message: "OpenRouter unavailable", code: "or_fixture" } }), {
            status: 502,
            headers: { "Content-Type": "application/json" },
          });
        }
        return sseResponse(baseSseChunks());
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
        const response = await handleResponses(openRouterResponsesRequest());
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "closed");
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.deepEqual(urls, [
      "https://openrouter.ai/api/v1/responses",
      "https://chatgpt.com/backend-api/codex/responses",
    ]);
    assert.equal(state.probe, null);
    assert.deepEqual(getResponseTelemetry(response)?.attemptedProviders, ["openrouter", "chatgpt_codex"]);
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "closed");
  });

  await t.step("failed early Codex recovery restores the OpenRouter error and reopens", async () => {
    const urls: string[] = [];
    const startedAtMs = Date.now();
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        if (url === "https://openrouter.ai/api/v1/responses") {
          return new Response(JSON.stringify({ error: { message: "OpenRouter unavailable", code: "or_fixture" } }), {
            status: 502,
            headers: { "Content-Type": "application/json", "Retry-After": "19" },
          });
        }
        return new Response(JSON.stringify({ error: { message: "Codex still unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        });
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
        return await handleResponses(openRouterResponsesRequest());
      },
      { openRouterApiKey: "or-test-key" },
    );

    const payload = await response.json() as { error?: { message?: string; code?: string } };
    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "open" && candidate.probe === null);
    assert.equal(response.status, 502);
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.equal(response.headers.get("Retry-After"), "19");
    assert.equal(payload.error?.message, "OpenRouter unavailable");
    assert.equal(payload.error?.code, "or_fixture");
    assert.deepEqual(urls, [
      "https://openrouter.ai/api/v1/responses",
      "https://chatgpt.com/backend-api/codex/responses",
    ]);
    assert.ok((state.open_until_ms ?? 0) >= startedAtMs + 119_000);
    assert.ok((state.open_until_ms ?? 0) <= Date.now() + 120_000);
    assert.equal(getResponseTelemetry(response)?.provider, "openrouter");
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "reopened");
  });

  await t.step("noneligible early Codex recovery restores the OpenRouter error and releases", async () => {
    const urls: string[] = [];
    const response = await withFetchMock(
      (url) => {
        urls.push(url);
        if (url === "https://openrouter.ai/api/v1/responses") {
          return new Response(JSON.stringify({ error: { message: "OpenRouter unavailable", code: "or_fixture" } }), {
            status: 502,
            headers: { "Content-Type": "application/json", "Retry-After": "23" },
          });
        }
        return new Response(JSON.stringify({ error: { message: "Codex request invalid", code: "codex_fixture" } }), {
          status: 400,
          headers: { "Content-Type": "application/json" },
        });
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
        return await handleResponses(openRouterResponsesRequest());
      },
      { openRouterApiKey: "or-test-key" },
    );

    const payload = await response.json() as { error?: { message?: string; code?: string } };
    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "open" && candidate.probe === null);
    assert.equal(response.status, 502);
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.equal(response.headers.get("Retry-After"), "23");
    assert.equal(payload.error?.message, "OpenRouter unavailable");
    assert.equal(payload.error?.code, "or_fixture");
    assert.deepEqual(urls, [
      "https://openrouter.ai/api/v1/responses",
      "https://chatgpt.com/backend-api/codex/responses",
    ]);
    assert.ok((state.open_until_ms ?? Number.MAX_SAFE_INTEGER) <= Date.now());
    assert.equal(getResponseTelemetry(response)?.provider, "openrouter");
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, "released");
  });

  await t.step("request cancellation releases a claimed half-open probe", async () => {
    const controller = new AbortController();
    const dispatched = new Deferred<void>();
    let upstreamCancelled = false;
    const response = await withFetchMock(
      (url) => {
        assert.equal(url, "https://chatgpt.com/backend-api/codex/responses");
        dispatched.resolve();
        return new Response(
          new ReadableStream<Uint8Array>({
            start(streamController) {
              streamController.enqueue(TEXT_ENCODER.encode(
                `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_cancel_probe" } })}\n\n`,
              ));
            },
            cancel() {
              upstreamCancelled = true;
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        );
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() - 1 });
        const responsePromise = handleResponses(openRouterResponsesRequest({}, controller.signal));
        await dispatched.promise;
        controller.abort(new DOMException("fixture cancellation", "AbortError"));
        return await responsePromise;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const state = await waitForOpenRouterCircuit((candidate) => candidate.phase === "open" && candidate.probe === null);
    assert.equal(response.status, 502);
    assert.equal(state.probe, null);
    assert.equal(upstreamCancelled, true);
  });
});

Deno.test("openai: direct OpenRouter dispatch commits the API-key provider", async () => {
  const nowMs = Date.now();
  const tokenHash = "openrouter-direct-dispatch-hash";
  const requestId = "openrouter-direct-dispatch-request";
  const record: ApiKeyHashRecord = {
    id: "openrouter-direct-dispatch-key",
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 2,
    usage_requests: 0,
    usage_reset_at_ms: nowMs + 60 * 60_000,
    window_ms: 60 * 60_000,
    usage_quota_version: 3,
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 0,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  const policy = apiKeyPolicyFromHashRecord(tokenHash, record, nowMs);
  assert.ok(policy);
  kvStore.set(keyToString(["ubq_ai", "api_keys", "hash", tokenHash]), record);
  const decision = await reserveApiKeyUsageV3(policy, requestId, "responses", { kv: kvStub, nowMs });
  assert.equal(decision.ok, true);
  if (!decision.ok) return;

  try {
    const response = await withFetchMock(
      (url) => {
        assert.equal(url, "https://openrouter.ai/api/v1/responses");
        return sseResponse(openRouterTextSseChunks());
      },
      async () => {
        seedOpenRouterCircuit({ phase: "open", openUntilMs: Date.now() + 60_000 });
        const response = await handleResponses(openRouterResponsesRequest(), {
          keyId: policy.key_id,
          kernelRepo: null,
          kernelOrg: null,
          requestId,
          startedAtMs: nowMs,
          beforeProviderDispatch: decision.reservation.beforeProviderDispatch,
        });
        await response.text();
        return response;
      },
      { openRouterApiKey: "or-test-key" },
    );

    const request = kvStore.get(keyToString(apiKeyUsageV3RequestKey(policy, requestId))) as
      | ApiKeyUsageRequestV3
      | undefined;
    const window = kvStore.get(keyToString(apiKeyUsageV3WindowKey(policy))) as
      | { committed_requests?: number; reserved_requests?: number }
      | undefined;
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "openrouter");
    assert.equal(request?.state, "dispatched");
    assert.equal(request?.provider, "openrouter");
    assert.equal(window?.committed_requests, 1);
    assert.equal(window?.reserved_requests, 0);
  } finally {
    kvStore.delete(keyToString(["ubq_ai", "api_keys", "hash", tokenHash]));
    kvStore.delete(keyToString(apiKeyUsageV3RequestKey(policy, requestId)));
    kvStore.delete(keyToString(apiKeyUsageV3WindowKey(policy)));
  }
});

Deno.test("openai: buffered Responses observe each real or synthetic terminal once", async (t) => {
  await t.step("real response.completed usage", async () => {
    const observations: Array<{ completed: boolean; totalTokens: number | null }> = [];
    const response = await withFetchMock(
      () => sseResponse(baseSseChunks()),
      () =>
        handleResponses(openRouterResponsesRequest({ stream: false }), {
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

  await t.step("synthetic response.failed usage", async () => {
    const observations: Array<{ completed: boolean; totalTokens: number | null }> = [];
    const response = await withFetchMock(
      (url) =>
        url === "https://openrouter.ai/api/v1/responses"
          ? sseResponse(openRouterTextSseChunks({ terminal: false }))
          : new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
            status: 503,
            headers: { "Content-Type": "application/json" },
          }),
      () =>
        handleResponses(openRouterResponsesRequest({ stream: false }), {
          keyId: null,
          kernelRepo: null,
          kernelOrg: null,
          onTerminalUsage: (usage, completed) =>
            observations.push({
              completed,
              totalTokens: usage?.totalTokens ?? null,
            }),
        }),
      { openRouterApiKey: "or-test-key" },
    );
    await response.text();
    assert.deepEqual(observations, [{ completed: false, totalTokens: null }]);
    assert.equal(getResponseTelemetry(response)?.streamTerminalType, "response.failed");
  });
});

Deno.test("openai: buffered committed Responses failures use the official server_error code", async () => {
  const response = await withFetchMock(
    () =>
      sseResponse([
        ...baseSseChunks().slice(0, -1),
        'data: {"type":\n\n',
      ]),
    () => handleResponses(openRouterResponsesRequest({ stream: false })),
  );
  assert.equal(response.status, 502);
  const payload = await response.json() as { error?: { code?: string } };
  assert.equal(payload.error?.code, "server_error");
});

Deno.test("openai: missing OpenRouter key leaves the global circuit untouched", async () => {
  const circuitKey = keyToString(["uos_ai", "openrouter_failover", "circuit", "v1"]);
  for (let request = 0; request < 2; request += 1) {
    const response = await withFetchMock(
      () =>
        new Response(JSON.stringify({ error: { message: "Primary unavailable" } }), {
          status: 503,
          headers: { "Content-Type": "application/json" },
        }),
      () => handleResponses(openRouterResponsesRequest()),
    );
    assert.equal(response.status, 503);
    assert.equal(getResponseTelemetry(response)?.openRouterCircuitTransition, null);
    assert.equal(kvStore.has(circuitKey), false);
  }
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
