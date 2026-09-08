import assert from "node:assert/strict";
import { apiKeyHashKey, apiKeyIdKey } from "../src/api_keys.ts";
import { ApiKeyQuotaDispatchError } from "../src/api_key_policy.ts";
import { CEREBRAS_CHAT_COMPLETIONS_URL, fetchCerebrasChatCompletions } from "../src/cerebras.ts";
import {
  CODEX_AUTH_POOL_KV_KEY,
  fetchCodexResponses,
  getCodexResponseAffinityOutcome,
  getCodexResponseSlot,
  markCodexResponseCompleted,
  resetCodexAuthCacheForTest,
} from "../src/codex.ts";
import { resetCodexAccountRoutingForTest } from "../src/codex_account_routing.ts";
import { config } from "../src/config.ts";
import { setKvForTest } from "../src/kv.ts";
import { fetchMeteredResponses, METERED_BASE_URL } from "../src/metered.ts";
import { resetProviderHealthThrottleForTest } from "../src/provider_health.ts";
import { resetRuntimeConfigCacheForTest, RUNTIME_CONFIG_V2_KEY } from "../src/runtime_config.ts";
import { fetchSurplusResponses, SURPLUS_BASE_URL, SurplusError } from "../src/surplus.ts";
import { decryptExportedSentinelReplay, type ExportedSentinelReplayCapture } from "../src/sentinel_replay_capture.ts";
import {
  createSentinelUpstreamRecorder,
  type SentinelUpstreamAttempt,
  type SentinelUpstreamContentType,
  type SentinelUpstreamProvider,
  type SentinelUpstreamTerminal,
  type SentinelUpstreamTrace,
} from "../src/sentinel_upstream_capture.ts";
import { createRecordedUpstreamReplay } from "./helpers/sentinel-recorded-upstream.ts";
import { base64UrlDecode, base64UrlEncode, encodeHex, sha256Base64Url } from "../src/utils.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const { default: handler } = await import("../src/handler.ts");

const kvAvailable = typeof Deno.openKv === "function";

const encoder = new TextEncoder();

const CODEX_RESPONSES_URL = `${config.codexBaseUrl}/responses`;
const CODEX_REFRESH_URL = "https://auth.openai.com/oauth/token";

/**
 * Trusted fixture route map: distinct exact HTTPS endpoint strings per provider
 * enum. Only synthetic public test data is committed; nothing here is a secret.
 */
const REPLAY_ROUTES: Readonly<Record<SentinelUpstreamProvider, string>> = Object.freeze({
  chatgpt_codex: CODEX_RESPONSES_URL,
  surplus: `${SURPLUS_BASE_URL}/v1/responses`,
  metered: `${METERED_BASE_URL}/v1/responses`,
  cerebras: CEREBRAS_CHAT_COMPLETIONS_URL,
});

const decodeChunks = (
  trace: ReturnType<ReturnType<typeof createSentinelUpstreamRecorder>["snapshotAndSeal"]>,
): string =>
  trace.attempts.flatMap((attempt) => attempt.chunks_base64.map((chunk) => base64UrlDecode(chunk)))
    .reduce((text, bytes) => text + new TextDecoder().decode(bytes), "");

const replayAttempt = (
  provider: SentinelUpstreamProvider,
  status: number | null,
  content_type: SentinelUpstreamContentType | null,
  terminal: SentinelUpstreamTerminal,
  chunksText: readonly string[] = [],
): SentinelUpstreamAttempt => ({
  provider,
  status,
  content_type,
  chunks_base64: chunksText.map((text) => btoa(text)),
  terminal,
});

const replayTrace = (
  attempts: readonly SentinelUpstreamAttempt[],
  flags: Partial<Pick<SentinelUpstreamTrace, "attempts_truncated" | "bytes_truncated" | "chunks_truncated">> = {},
): SentinelUpstreamTrace => ({
  version: 1,
  attempts,
  attempts_truncated: false,
  bytes_truncated: false,
  chunks_truncated: false,
  ...flags,
});

Deno.test({
  name: "actual Surplus fetch captures pre-normalization SSE bytes and stays lazy until the consumer reads",
  async fn() {
    const recorder = createSentinelUpstreamRecorder();
    const rawChunks: string[] = [
      `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_raw", created_at: 0 } })}\n\n`,
      `data: ${
        JSON.stringify({ type: "response.output_text.delta", output_index: 0, content_index: 0, delta: "hello" })
      }\n\n`,
      `data: ${
        JSON.stringify({
          type: "response.completed",
          response: { id: "resp_raw", model: "surplus-fixture", output: [] },
        })
      }\n\n`,
    ];
    const rawText = rawChunks.join("");
    let pulls = 0;
    const pending = [...rawChunks];
    const fetcher = (_url: string | URL | Request, _init?: RequestInit): Promise<Response> =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            pull(controller) {
              pulls += 1;
              const next = pending.length ? pending.shift()! : null;
              if (next === null) {
                controller.close();
                return;
              }
              controller.enqueue(encoder.encode(next));
            },
          }, { highWaterMark: 0 }),
          { status: 200, headers: { "Content-Type": "text/event-stream", "X-Request-Id": "surplus-raw-1" } },
        ),
      );
    const result = await fetchSurplusResponses(
      { model: "surplus-fixture", input: "hello" },
      { apiKey: "surplus-fixture-key", fetcher, sentinelUpstreamRecorder: recorder },
    );
    assert.equal(result.request_id, "surplus-raw-1");
    assert.equal(result.response.status, 200);
    // The normalizer's existing ReadableStream may prefetch exactly one chunk
    // (its own default high-water-mark); the recorder itself never reads
    // ahead, which the recorder-level lazy test proves directly. No drain may
    // occur here.
    assert.equal(pulls <= 1, true, "the provider must not drain the raw upstream bytes");
    const normalizedText = await result.response.text();
    assert.equal(pulls, rawChunks.length, "each raw chunk is read exactly once");
    const trace = recorder.snapshotAndSeal();
    assert.equal(trace.attempts.length, 1);
    const attempt = trace.attempts[0]!;
    assert.equal(attempt.provider, "surplus");
    assert.equal(attempt.status, 200);
    assert.equal(attempt.content_type, "text/event-stream");
    // The normalizer returns after the parsed terminal event and cancels the
    // source, so this is a consumed prefix: cancelled is the truthful
    // terminal, never a claim of complete raw network EOF.
    assert.equal(attempt.terminal, "cancelled");
    // The captured bytes are the raw pre-normalization SSE framing, while the
    // returned body is the normalizer's re-encoded event stream.
    assert.equal(decodeChunks(trace), rawText, "captured bytes must be the exact raw upstream SSE");
    assert.equal(decodeChunks(trace).startsWith("data: "), true);
    assert.equal(normalizedText.startsWith("event: response.created"), true);
    assert.equal(normalizedText.includes("output_text.done"), true);
    recorder.dispose();
  },
});

Deno.test("actual Surplus no-header fetch failure records fetch_error without inventing headers", async () => {
  const recorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchSurplusResponses(
      { model: "surplus-fixture", input: "hello" },
      {
        apiKey: "surplus-fixture-key",
        fetcher: () => Promise.reject(new TypeError("surplus socket closed")),
        sentinelUpstreamRecorder: recorder,
      },
    ),
    (error: unknown) => error instanceof SurplusError && error.code === "surplus_upstream_unreachable",
  );
  const trace = recorder.snapshotAndSeal();
  assert.equal(trace.attempts.length, 1);
  assert.deepEqual(
    trace.attempts.map(({ status, content_type, terminal, chunks_base64 }) => ({
      status,
      content_type,
      terminal,
      chunks_base64,
    })),
    [{ status: null, content_type: null, terminal: "fetch_error", chunks_base64: [] }],
  );
  recorder.dispose();
});

Deno.test("quota denial never opens a dispatch slot on any paid provider", async () => {
  const quota = new ApiKeyQuotaDispatchError("quota denied", {
    status: 429,
    code: "quota_denied",
    errorType: "server_error",
  });
  let dispatchAttempts = 0;
  const untouchedFetcher = (): Promise<Response> => {
    dispatchAttempts += 1;
    return Promise.reject(new Error("must not dispatch"));
  };

  const surplusRecorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchSurplusResponses(
      { model: "surplus-fixture", input: "hello" },
      {
        apiKey: "surplus-fixture-key",
        beforeDispatch: () => Promise.reject(quota),
        fetcher: untouchedFetcher,
        sentinelUpstreamRecorder: surplusRecorder,
      },
    ),
    ApiKeyQuotaDispatchError,
  );
  assert.equal(surplusRecorder.snapshotAndSeal().attempts.length, 0);

  const meteredRecorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchMeteredResponses(
      { model: "metered-fixture", input: "hello" },
      {
        apiKey: "metered-fixture-key",
        beforeDispatch: () => Promise.reject(quota),
        fetcher: untouchedFetcher,
        sentinelUpstreamRecorder: meteredRecorder,
      },
    ),
    ApiKeyQuotaDispatchError,
  );
  assert.equal(meteredRecorder.snapshotAndSeal().attempts.length, 0);

  const cerebrasRecorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchCerebrasChatCompletions(
      { model: "gpt-oss-120b", messages: [{ role: "user", content: "hello" }] },
      {
        apiKey: "cerebras-fixture-key",
        beforeDispatch: () => Promise.reject(quota),
        fetcher: untouchedFetcher,
        sentinelUpstreamRecorder: cerebrasRecorder,
      },
    ),
    ApiKeyQuotaDispatchError,
  );
  assert.equal(cerebrasRecorder.snapshotAndSeal().attempts.length, 0);
  assert.equal(dispatchAttempts, 0, "no provider fetch may run after admission denial");
});

Deno.test("actual Metered dispatch returns the exact upstream body and bounded headers", async () => {
  const recorder = createSentinelUpstreamRecorder();
  const rawSse =
    `data: ${JSON.stringify({ type: "response.created", response: { id: "resp_metered", created_at: 0 } })}\n\n` +
    `data: ${JSON.stringify({ type: "response.completed", response: { id: "resp_metered" } })}\n\n`;
  const fetcher = (): Promise<Response> =>
    Promise.resolve(
      new Response(rawSse, {
        status: 200,
        headers: { "Content-Type": "text/event-stream", "X-Oneapi-Request-Id": "metered-id-1" },
      }),
    );
  const result = await fetchMeteredResponses(
    { model: "metered-fixture", input: "hello" },
    { apiKey: "metered-fixture-key", fetcher, sentinelUpstreamRecorder: recorder },
  );
  assert.equal(result.request_id, "metered-id-1");
  assert.equal(result.response.status, 200);
  assert.equal(await result.response.text(), rawSse);
  const attempt = recorder.snapshotAndSeal().attempts[0]!;
  assert.equal(attempt.provider, "metered");
  assert.equal(attempt.status, 200);
  assert.equal(attempt.content_type, "text/event-stream");
  assert.equal(attempt.terminal, "eof");
  recorder.dispose();

  const noHeaderRecorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchMeteredResponses(
      { model: "metered-fixture", input: "hello" },
      {
        apiKey: "metered-fixture-key",
        fetcher: () => Promise.reject(new TypeError("metered socket closed")),
        sentinelUpstreamRecorder: noHeaderRecorder,
      },
    ),
    (error: unknown) => error instanceof Error && error.name === "MeteredError",
  );
  assert.equal(noHeaderRecorder.snapshotAndSeal().attempts[0]?.terminal, "fetch_error");
  noHeaderRecorder.dispose();
});

Deno.test("actual Cerebras dispatch returns the exact upstream body and records the attempt", async () => {
  const recorder = createSentinelUpstreamRecorder();
  const rawJson = JSON.stringify({ id: "cerebras-1", object: "chat.completion", choices: [{ index: 0 }] });
  const fetcher = (): Promise<Response> =>
    Promise.resolve(new Response(rawJson, { status: 200, headers: { "Content-Type": "application/json" } }));
  const response = await fetchCerebrasChatCompletions(
    { model: "gpt-oss-120b", messages: [{ role: "user", content: "hello" }] },
    { apiKey: "cerebras-fixture-key", fetcher, sentinelUpstreamRecorder: recorder },
  );
  assert.equal(response.status, 200);
  assert.equal(await response.text(), rawJson);
  const attempt = recorder.snapshotAndSeal().attempts[0]!;
  assert.equal(attempt.provider, "cerebras");
  assert.equal(attempt.status, 200);
  assert.equal(attempt.content_type, "application/json");
  assert.equal(attempt.terminal, "eof");
  recorder.dispose();

  const noHeaderRecorder = createSentinelUpstreamRecorder();
  await assert.rejects(
    fetchCerebrasChatCompletions(
      { model: "gpt-oss-120b", messages: [{ role: "user", content: "hello" }] },
      {
        apiKey: "cerebras-fixture-key",
        fetcher: () => Promise.reject(new TypeError("cerebras socket closed")),
        sentinelUpstreamRecorder: noHeaderRecorder,
      },
    ),
    (error: unknown) => error instanceof Error && error.name === "CerebrasError",
  );
  assert.equal(noHeaderRecorder.snapshotAndSeal().attempts[0]?.terminal, "fetch_error");
  noHeaderRecorder.dispose();
});

Deno.test({
  name: "actual Codex intermediate retries each count as an attempt and the wrapper is the WeakMap identity",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const originalFetch = globalThis.fetch;
    const originalNow = Date.now;
    const nowMs = 1_700_000_000_000;
    Date.now = () => nowMs;
    const base64Url = (value: unknown): string =>
      btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
    const auth = (label: string): CodexAuthState => ({
      access_token: `${base64Url({ alg: "none" })}.${base64Url({ exp: (nowMs + 60 * 60_000) / 1_000 })}.${label}`,
      refresh_token: `refresh-${label}`,
      account_id: `account-${label}`,
      updated_at_ms: nowMs,
    });
    const pool = (...accounts: CodexAuthState[]): CodexAuthPoolState => ({ accounts, updated_at_ms: nowMs });
    await kv.set(CODEX_AUTH_POOL_KV_KEY, pool(auth("one"), auth("two")));
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    resetProviderHealthThrottleForTest();
    const accountIds: string[] = [];
    let shouldQuotaFailOne = false;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      if (request.url === CODEX_REFRESH_URL) {
        return Promise.resolve(new Response(JSON.stringify({ access_token: "refreshed-one" }), { status: 200 }));
      }
      const accountId = request.headers.get("ChatGPT-Account-ID") ?? "";
      accountIds.push(accountId);
      if (shouldQuotaFailOne && accountId === "account-one") {
        return Promise.resolve(
          new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": new Date(nowMs + 60_000).toUTCString() },
          }),
        );
      }
      return Promise.resolve(new Response("{}", { status: 200 }));
    };
    const body = () => ({ model: "gpt-5.6-luna", prompt_cache_key: "cache-recorder-weakmap", input: "fixture" });
    try {
      const recorder = createSentinelUpstreamRecorder();
      const first = await fetchCodexResponses(body(), {
        cacheScope: "api-key:recorder-weakmap",
        sentinelUpstreamRecorder: recorder,
      });
      assert.equal(getCodexResponseSlot(first), 1, "the wrapper must be the registered WeakMap identity");
      await markCodexResponseCompleted(first);
      recorder.dispose();
      assert.equal(recorder.snapshotAndSeal().attempts.length, 1);

      shouldQuotaFailOne = true;
      const remapRecorder = createSentinelUpstreamRecorder();
      const remapped = await fetchCodexResponses(body(), {
        cacheScope: "api-key:recorder-weakmap",
        sentinelUpstreamRecorder: remapRecorder,
      });
      shouldQuotaFailOne = false;
      assert.equal(remapped.status, 200);
      assert.equal(getCodexResponseSlot(remapped), 2, "account two is the canonical wrapper identity");
      assert.equal(getCodexResponseAffinityOutcome(remapped), "remapped");
      await remapped.text();
      const trace = remapRecorder.snapshotAndSeal();
      assert.deepEqual(
        trace.attempts.map(({ provider, status, terminal }) => ({ provider, status, terminal })),
        [
          { provider: "chatgpt_codex", status: 429, terminal: "eof" },
          { provider: "chatgpt_codex", status: 200, terminal: "eof" },
        ],
        "each intermediate retry counts as its own attempt in dispatch order",
      );
      assert.equal(
        trace.attempts[0]!.chunks_base64.length > 0,
        true,
        "the 429 body read for classification is raw evidence",
      );
      remapRecorder.dispose();
    } finally {
      globalThis.fetch = originalFetch;
      Date.now = originalNow;
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      resetProviderHealthThrottleForTest();
      await kv.close();
      setKvForTest(null);
    }
  },
});

const SUPER_ADMIN_TOKEN = "sentinel-upstream-provider-super-admin";
const MARKER = "sentinel-upstream-handler-marker-71c3e59a";
const RAW_UPSTREAM_BODY = JSON.stringify({
  error: { message: "Codex fixture upstream failure", type: "server_error", code: "fixture_500", param: null },
});
const CAPTURE_BODY = new TextEncoder().encode(JSON.stringify({
  model: "gpt-5.6-sol",
  stream: false,
  input: MARKER,
}));

const exportUrl = (params: Record<string, string>): string =>
  `https://ai.ubq.fi/admin/sentinel/replay-captures?${new URLSearchParams(params)}`;

const seedAuthenticatedKey = async (kv: Deno.Kv, token: string): Promise<void> => {
  const now = Date.now();
  const hash = await sha256Base64Url(token);
  const id = `upstream-provider-${hash.slice(0, 12)}`;
  const policyBase = {
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 100,
    usage_requests: 0,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    usage_quota_version: 3,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 1_000_000,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  await kv.set(apiKeyHashKey(hash), { id, ...policyBase });
  await kv.set(apiKeyIdKey(id), {
    id,
    name: "sentinel upstream provider test key",
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: now,
    ...policyBase,
    paid_fallback_model_ids: ["gpt-5.6-sol"],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { "gpt-5.6-sol": 250_000 },
    paid_fallback_pricing_checked_at_ms: now,
  });
};

Deno.test({
  name: "real handler failure captures exact raw Codex upstream bytes through encrypted export and decrypt",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    const keyBytes = crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;
    Deno.env.set("SENTINEL_REPLAY_KEY", base64UrlEncode(keyBytes));
    const token = `u_${encodeHex(crypto.getRandomValues(new Uint8Array(32)))}`;
    const originalFetch = globalThis.fetch;
    const originalNow = Date.now;
    const nowMs = 1_700_000_000_000;
    Date.now = () => nowMs;
    const base64Url = (value: unknown): string =>
      btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
    const accessToken = `${base64Url({ alg: "none" })}.${
      base64Url({ exp: (nowMs + 60 * 60_000) / 1_000 })
    }.handler-fixture`;
    const authState: CodexAuthState = {
      access_token: accessToken,
      refresh_token: "refresh-handler-fixture",
      account_id: "account-handler-fixture",
      updated_at_ms: nowMs,
    };
    const authPool: CodexAuthPoolState = { accounts: [authState], updated_at_ms: nowMs };
    const runtimeConfig = {
      version: 2,
      default_model: "gpt-5.6-sol",
      default_reasoning_effort: "low",
      codex_models: {
        source: "chatgpt_codex",
        client_version: "0.100.0",
        updated_at_ms: nowMs,
        models: [{
          slug: "gpt-5.6-sol",
          default_reasoning_level: "low",
          supported_reasoning_levels: ["none", "low", "medium", "high"],
        }],
      },
      updated_at_ms: nowMs,
    };
    await Promise.all([
      kv.set(CODEX_AUTH_POOL_KV_KEY, authPool),
      kv.set(RUNTIME_CONFIG_V2_KEY, runtimeConfig),
    ]);
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    resetRuntimeConfigCacheForTest();
    resetProviderHealthThrottleForTest();
    let upstreamDispatches = 0;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      if (request.url === CODEX_REFRESH_URL) {
        return Promise.resolve(new Response(JSON.stringify({ access_token: accessToken }), { status: 200 }));
      }
      if (request.url === CODEX_RESPONSES_URL) {
        upstreamDispatches += 1;
        return Promise.resolve(
          new Response(RAW_UPSTREAM_BODY, {
            status: 500,
            headers: { "Content-Type": "application/json" },
          }),
        );
      }
      if (request.url.includes("openai.com")) {
        return Promise.resolve(new Response(JSON.stringify({ error: "fixture" }), { status: 400 }));
      }
      throw new Error(`Unexpected handler test fetch: ${request.url}`);
    };
    try {
      await seedAuthenticatedKey(kv, token);
      const response = await handler(
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
          body: CAPTURE_BODY,
        }),
      );
      assert.equal(response.status, 500);
      assert.equal(upstreamDispatches, 1, "one real Codex dispatch must drive the handler failure");
      const responseText = await response.text();
      assert.equal(responseText.includes(MARKER), false, "failure response leaks request plaintext");

      const page = await handler(
        new Request(exportUrl({ after_ms: "0", before_ms: String(Date.now() + 2_000) }), {
          headers: { Authorization: `Bearer ${SUPER_ADMIN_TOKEN}` },
        }),
      );
      assert.equal(page.status, 200);
      const exported = await page.json() as { data: ExportedSentinelReplayCapture[]; cursor: string | null };
      assert.equal(exported.data.length, 1);
      const capture = exported.data[0]!;
      const plaintext = await decryptExportedSentinelReplay(capture, keyBytes);
      assert.equal(plaintext.version, 2);
      assert.equal(plaintext.endpoint, "/v1/responses");
      assert.equal(plaintext.method, "POST");
      assert.deepEqual([...plaintext.body], [...CAPTURE_BODY]);
      assert.equal(new TextDecoder().decode(plaintext.body), new TextDecoder().decode(CAPTURE_BODY));
      assert.equal(plaintext.observation.status, 500);
      assert.equal(plaintext.upstream.attempts.length, 1);
      const attempt = plaintext.upstream.attempts[0]!;
      assert.equal(attempt.provider, "chatgpt_codex");
      assert.equal(attempt.status, 500);
      assert.equal(attempt.content_type, "application/json");
      assert.equal(attempt.terminal, "eof");
      assert.deepEqual(
        attempt.chunks_base64.map((chunk) => new TextDecoder().decode(base64UrlDecode(chunk))).join(""),
        RAW_UPSTREAM_BODY,
        "the decrypted private plaintext must carry the exact raw upstream bytes",
      );
      assert.equal(JSON.stringify(exported).includes("account-handler-fixture"), false);
      assert.equal(JSON.stringify(exported).includes(RAW_UPSTREAM_BODY), false, "export never carries plaintext");
    } finally {
      Deno.env.delete("SENTINEL_REPLAY_KEY");
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      globalThis.fetch = originalFetch;
      Date.now = originalNow;
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      resetRuntimeConfigCacheForTest();
      resetProviderHealthThrottleForTest();
      await kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test("recorded upstream replay preserves exact chunk order and boundaries at EOF", async () => {
  const originalFetch = globalThis.fetch;
  const inputTrace = replayTrace([
    replayAttempt("metered", 200, "application/json", "eof", ["abc", "def", "ghi"]),
  ]);
  const inputBefore = JSON.stringify(inputTrace);
  const replay = createRecordedUpstreamReplay(inputTrace, REPLAY_ROUTES);
  const response = await replay.fetch(REPLAY_ROUTES.metered);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "application/json");
  const reader = response.body!.getReader();
  const chunks: string[] = [];
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(new TextDecoder().decode(value));
  }
  assert.deepEqual(chunks, ["abc", "def", "ghi"], "each original chunk must be a separate exact boundary");
  replay.assertComplete();
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
  assert.equal(JSON.stringify(inputTrace), inputBefore, "the input trace must stay unchanged");
  assert.equal(globalThis.fetch, originalFetch, "the helper must never replace or invoke global fetch");
});

Deno.test("recorded upstream read_error delivers the prefix then errors on the next pull", async () => {
  const replay = createRecordedUpstreamReplay(
    replayTrace([replayAttempt("surplus", 200, "text/event-stream", "read_error", ["x"])]),
    REPLAY_ROUTES,
  );
  const response = await replay.fetch(REPLAY_ROUTES.surplus);
  assert.equal(response.headers.get("content-type"), "text/event-stream");
  const reader = response.body!.getReader();
  const first = await reader.read();
  assert.equal(first.done, false);
  assert.equal(new TextDecoder().decode(first.value), "x");
  await assert.rejects(reader.read(), (error: unknown) => error instanceof TypeError);
  replay.assertComplete();
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
});

Deno.test("recorded upstream fetch_error rejects with no headers or chunks and completes the attempt", async () => {
  const replay = createRecordedUpstreamReplay(
    replayTrace([replayAttempt("cerebras", null, null, "fetch_error")]),
    REPLAY_ROUTES,
  );
  await assert.rejects(replay.fetch(REPLAY_ROUTES.cerebras), (error: unknown) => error instanceof TypeError);
  replay.assertComplete();
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
});

Deno.test("recorded upstream cancelled completes only when the consumer cancels after the full prefix", async () => {
  const replay = createRecordedUpstreamReplay(
    replayTrace([replayAttempt("surplus", 200, "text/event-stream", "cancelled", ["data:", "x\n"])]),
    REPLAY_ROUTES,
  );
  const response = await replay.fetch(REPLAY_ROUTES.surplus);
  const reader = response.body!.getReader();
  assert.equal(new TextDecoder().decode((await reader.read()).value), "data:");
  assert.equal(new TextDecoder().decode((await reader.read()).value), "x\n");
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 0, failed: false });
  await reader.cancel();
  replay.assertComplete();
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
});

Deno.test("recorded upstream cancelled prefix rejects when read beyond it, never inventing EOF", async () => {
  const replay = createRecordedUpstreamReplay(
    replayTrace([replayAttempt("surplus", 200, "text/event-stream", "cancelled", ["data:"])]),
    REPLAY_ROUTES,
  );
  const response = await replay.fetch(REPLAY_ROUTES.surplus);
  const reader = response.body!.getReader();
  assert.equal(new TextDecoder().decode((await reader.read()).value), "data:");
  await assert.rejects(reader.read(), (error: unknown) => error instanceof TypeError);
  assert.throws(() => replay.assertComplete(), /replay failed/);
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 0, failed: true });
  await assert.rejects(replay.fetch(REPLAY_ROUTES.surplus), (error: unknown) => error instanceof TypeError);
});

Deno.test("recorded upstream cancelled prefix refuses early consumer cancellation", async () => {
  const replay = createRecordedUpstreamReplay(
    replayTrace([replayAttempt("surplus", 200, "text/event-stream", "cancelled", ["data:", "x\n"])]),
    REPLAY_ROUTES,
  );
  const response = await replay.fetch(REPLAY_ROUTES.surplus);
  const reader = response.body!.getReader();
  await reader.read();
  await reader.cancel();
  assert.throws(() => replay.assertComplete(), /replay failed/);
  assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 0, failed: true });
});

Deno.test("recorded upstream enforces exact ordered provider routes and permanent failure", async () => {
  const twoAttempts = replayTrace([
    replayAttempt("chatgpt_codex", 200, "application/json", "eof", ["{}"]),
    replayAttempt("surplus", 200, "text/event-stream", "eof", ["{}"]),
  ]);
  // Unexpected URL entirely outside the route map.
  const unexpected = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES);
  await assert.rejects(unexpected.fetch("https://unmatched.example/nope"), TypeError);
  assert.throws(() => unexpected.assertComplete(), /replay failed/);
  assert.deepEqual(unexpected.snapshot(), { attemptsDispatched: 0, attemptsCompleted: 0, failed: true });
  // Wrong provider route while codex is the next recorded attempt.
  const wrongProvider = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES);
  await assert.rejects(wrongProvider.fetch(REPLAY_ROUTES.surplus), TypeError);
  assert.deepEqual(wrongProvider.snapshot(), { attemptsDispatched: 0, attemptsCompleted: 0, failed: true });
  // Order mismatch: the second codex call cannot satisfy the surplus attempt.
  const wrongOrder = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES);
  const first = await wrongOrder.fetch(REPLAY_ROUTES.chatgpt_codex);
  const firstReader = first.body!.getReader();
  await firstReader.read();
  await firstReader.read();
  await assert.rejects(wrongOrder.fetch(REPLAY_ROUTES.chatgpt_codex), TypeError);
  assert.deepEqual(wrongOrder.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: true });
  // Dispatched but unconsumed bodies are not completion.
  const unconsumed = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES);
  await unconsumed.fetch(REPLAY_ROUTES.chatgpt_codex);
  await unconsumed.fetch(REPLAY_ROUTES.surplus);
  assert.deepEqual(unconsumed.snapshot(), { attemptsDispatched: 2, attemptsCompleted: 0, failed: false });
  assert.throws(() => unconsumed.assertComplete(), /replay is incomplete/);
  // Extra dispatch after full completion is a permanent failure.
  const extra = createRecordedUpstreamReplay(twoAttempts, REPLAY_ROUTES);
  await (await extra.fetch(REPLAY_ROUTES.chatgpt_codex)).text();
  await (await extra.fetch(REPLAY_ROUTES.surplus)).text();
  extra.assertComplete();
  await assert.rejects(extra.fetch(REPLAY_ROUTES.chatgpt_codex), TypeError);
  assert.throws(() => extra.assertComplete(), /replay failed/);
  assert.deepEqual(extra.snapshot(), { attemptsDispatched: 2, attemptsCompleted: 2, failed: true });
});

Deno.test("recorded upstream refuses unavailable or non-reproducible replay evidence at creation", () => {
  const usable = replayAttempt("metered", 200, "application/json", "eof", ["{}"]);
  assert.throws(() => createRecordedUpstreamReplay(replayTrace([]), REPLAY_ROUTES), /trace has no attempts/);
  assert.throws(
    () => createRecordedUpstreamReplay(replayTrace([usable], { attempts_truncated: true }), REPLAY_ROUTES),
    /trace is truncated/,
  );
  assert.throws(
    () => createRecordedUpstreamReplay(replayTrace([usable], { bytes_truncated: true }), REPLAY_ROUTES),
    /trace is truncated/,
  );
  assert.throws(
    () => createRecordedUpstreamReplay(replayTrace([usable], { chunks_truncated: true }), REPLAY_ROUTES),
    /trace is truncated/,
  );
  assert.throws(
    () =>
      createRecordedUpstreamReplay(
        replayTrace([replayAttempt("metered", 200, "application/json", "pending")]),
        REPLAY_ROUTES,
      ),
    /attempt is pending/,
  );
  assert.throws(
    () => createRecordedUpstreamReplay(replayTrace([replayAttempt("metered", null, null, "pending")]), REPLAY_ROUTES),
    /attempt is pending/,
  );
  assert.throws(
    () =>
      createRecordedUpstreamReplay(replayTrace([replayAttempt("metered", 200, "other", "eof", ["{}"])]), REPLAY_ROUTES),
    /MIME/,
  );
  assert.throws(
    () =>
      createRecordedUpstreamReplay(
        replayTrace([replayAttempt("metered", 101, "text/event-stream", "eof")]),
        REPLAY_ROUTES,
      ),
    /informational/,
  );
  assert.throws(
    () =>
      createRecordedUpstreamReplay(
        replayTrace([replayAttempt("metered", 204, "application/json", "eof", ["{}"])]),
        REPLAY_ROUTES,
      ),
    /bodyless/,
  );
  assert.throws(
    () =>
      createRecordedUpstreamReplay(
        replayTrace([replayAttempt("metered", 204, "application/json", "read_error")]),
        REPLAY_ROUTES,
      ),
    /bodyless/,
  );
  const ambiguous = { ...REPLAY_ROUTES, surplus: REPLAY_ROUTES.chatgpt_codex };
  assert.throws(
    () => createRecordedUpstreamReplay(replayTrace([usable]), ambiguous),
    /not unique/,
  );
  const insecure = { ...REPLAY_ROUTES, metered: "http://replay.fixture.example/metered" };
  assert.throws(() => createRecordedUpstreamReplay(replayTrace([usable]), insecure), /exact HTTPS endpoint/);
  const credential = { ...REPLAY_ROUTES, surplus: "https://user:pass@replay.fixture.example/surplus" };
  assert.throws(() => createRecordedUpstreamReplay(replayTrace([usable]), credential), /exact HTTPS endpoint/);
});

Deno.test("recorded upstream bodyless 204/205/304 replay as null body completed at dispatch", async () => {
  for (const status of [204, 205, 304] as const) {
    const replay = createRecordedUpstreamReplay(
      replayTrace([replayAttempt("metered", status, "other", "eof")]),
      REPLAY_ROUTES,
    );
    const response = await replay.fetch(REPLAY_ROUTES.metered);
    assert.equal(response.status, status);
    assert.equal(response.body, null);
    replay.assertComplete();
    assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
  }
});

Deno.test({
  name: "real handler replays the decrypted recorded Codex raw failure through the trusted replay transport",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    const keyBytes = crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;
    const priorReplayKey = Deno.env.get("SENTINEL_REPLAY_KEY");
    Deno.env.set("SENTINEL_REPLAY_KEY", base64UrlEncode(keyBytes));
    const token = `u_${encodeHex(crypto.getRandomValues(new Uint8Array(32)))}`;
    const originalFetch = globalThis.fetch;
    const originalNow = Date.now;
    const nowMs = 1_700_000_000_000;
    Date.now = () => nowMs;
    const base64Url = (value: unknown): string =>
      btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
    const accessToken = `${base64Url({ alg: "none" })}.${
      base64Url({ exp: (nowMs + 60 * 60_000) / 1_000 })
    }.handler-fixture`;
    const authState: CodexAuthState = {
      access_token: accessToken,
      refresh_token: "refresh-handler-fixture",
      account_id: "account-handler-fixture",
      updated_at_ms: nowMs,
    };
    const authPool: CodexAuthPoolState = { accounts: [authState], updated_at_ms: nowMs };
    const runtimeConfig = {
      version: 2,
      default_model: "gpt-5.6-sol",
      default_reasoning_effort: "low",
      codex_models: {
        source: "chatgpt_codex",
        client_version: "0.100.0",
        updated_at_ms: nowMs,
        models: [{
          slug: "gpt-5.6-sol",
          default_reasoning_level: "low",
          supported_reasoning_levels: ["none", "low", "medium", "high"],
        }],
      },
      updated_at_ms: nowMs,
    };
    await Promise.all([
      kv.set(CODEX_AUTH_POOL_KV_KEY, authPool),
      kv.set(RUNTIME_CONFIG_V2_KEY, runtimeConfig),
    ]);
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    resetRuntimeConfigCacheForTest();
    resetProviderHealthThrottleForTest();
    let rawDispatches = 0;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      if (request.url === CODEX_REFRESH_URL) {
        return Promise.resolve(new Response(JSON.stringify({ access_token: accessToken }), { status: 200 }));
      }
      if (request.url === CODEX_RESPONSES_URL) {
        rawDispatches += 1;
        return Promise.resolve(
          new Response(RAW_UPSTREAM_BODY, {
            status: 500,
            headers: { "Content-Type": "application/json" },
          }),
        );
      }
      if (request.url.includes("openai.com")) {
        return Promise.resolve(new Response(JSON.stringify({ error: "fixture" }), { status: 400 }));
      }
      throw new Error(`Unexpected handler test fetch: ${request.url}`);
    };
    try {
      await seedAuthenticatedKey(kv, token);
      const gatewayRequest = (): Request =>
        new Request("https://ai.ubq.fi/v1/responses", {
          method: "POST",
          headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
          body: CAPTURE_BODY,
        });
      const originalResponse = await handler(gatewayRequest());
      assert.equal(originalResponse.status, 500);
      assert.equal(rawDispatches, 1, "one real Codex dispatch must drive the original failure");
      const originalText = await originalResponse.text();
      assert.equal(originalText.includes(MARKER), false, "failure response leaks request plaintext");

      const page = await handler(
        new Request(exportUrl({ after_ms: "0", before_ms: String(Date.now() + 2_000) }), {
          headers: { Authorization: `Bearer ${SUPER_ADMIN_TOKEN}` },
        }),
      );
      assert.equal(page.status, 200);
      const exported = await page.json() as { data: ExportedSentinelReplayCapture[]; cursor: string | null };
      assert.equal(exported.data.length, 1);
      const capture = exported.data[0]!;
      const plaintext = await decryptExportedSentinelReplay(capture, keyBytes);
      assert.equal(plaintext.endpoint, "/v1/responses");
      assert.equal(plaintext.method, "POST");
      assert.deepEqual([...plaintext.body], [...CAPTURE_BODY]);
      const replay = createRecordedUpstreamReplay(plaintext.upstream, REPLAY_ROUTES);
      const upstreamBefore = JSON.stringify(plaintext.upstream);

      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      resetRuntimeConfigCacheForTest();
      resetProviderHealthThrottleForTest();
      let replayedDispatches = 0;
      globalThis.fetch = (input, init) => {
        const url = input instanceof Request ? input.url : new URL(String(input)).href;
        if (url === CODEX_REFRESH_URL) {
          return Promise.resolve(new Response(JSON.stringify({ access_token: accessToken }), { status: 200 }));
        }
        replayedDispatches += 1;
        return replay.fetch(input, init);
      };
      const replayedResponse = await handler(
        new Request(`https://ai.ubq.fi${plaintext.endpoint}`, {
          method: plaintext.method,
          headers: {
            ...plaintext.compatibility_headers,
            Authorization: `Bearer ${token}`,
            ...(plaintext.content_type ? { "Content-Type": plaintext.content_type } : {}),
          },
          body: plaintext.body,
        }),
      );
      assert.equal(replayedResponse.status, originalResponse.status);
      assert.equal(
        replayedResponse.headers.get("x-uos-upstream"),
        originalResponse.headers.get("x-uos-upstream"),
        "the replayed failure must be attributed to the same upstream provider",
      );
      const replayedText = await replayedResponse.text();
      assert.equal(replayedText.includes(MARKER), false, "replayed failure response leaks request plaintext");
      assert.deepEqual(
        JSON.parse(replayedText) as unknown,
        JSON.parse(originalText) as unknown,
        "the real handler must reconstruct the exact original raw-failure semantics",
      );
      assert.equal(replayedDispatches, 1, "exactly one replayed upstream attempt must dispatch");
      replay.assertComplete();
      assert.deepEqual(replay.snapshot(), { attemptsDispatched: 1, attemptsCompleted: 1, failed: false });
      assert.equal(JSON.stringify(plaintext.upstream), upstreamBefore, "decrypted input trace must stay unchanged");

      const rePage = await handler(
        new Request(exportUrl({ after_ms: "0", before_ms: String(Date.now() + 2_000) }), {
          headers: { Authorization: `Bearer ${SUPER_ADMIN_TOKEN}` },
        }),
      );
      const reExported = await rePage.json() as { data: ExportedSentinelReplayCapture[]; cursor: string | null };
      assert.deepEqual(
        reExported.data[0],
        exported.data[0],
        "identical replayed raw evidence must deduplicate and leave the original capture unchanged",
      );
    } finally {
      if (priorReplayKey === undefined) Deno.env.delete("SENTINEL_REPLAY_KEY");
      else Deno.env.set("SENTINEL_REPLAY_KEY", priorReplayKey);
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      globalThis.fetch = originalFetch;
      Date.now = originalNow;
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      resetRuntimeConfigCacheForTest();
      resetProviderHealthThrottleForTest();
      await kv.close();
      setKvForTest(null);
    }
  },
});
