import assert from "node:assert/strict";
import { apiKeyHashKey, apiKeyIdKey } from "../src/api_keys.ts";
import { config } from "../src/config.ts";
import { DEBUG_ROUTING_KEY } from "../src/debug_routing.ts";
import { withTerminalRequestLog } from "../src/handler.ts";
import { setKvForTest } from "../src/kv.ts";
import {
  type AcceptedSentinelReplayInput,
  decryptExportedSentinelReplay,
  type ExportedSentinelReplayCapture,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
} from "../src/sentinel_replay_capture.ts";
import { base64UrlEncode, encodeHex, sha256Base64Url } from "../src/utils.ts";

const { default: handler } = await import("../src/handler.ts");

const kvAvailable = typeof Deno.openKv === "function";

const SUPER_ADMIN_TOKEN = "sentinel-request-capture-super-admin";
const MARKER = "sentinel-request-capture-exact-bytes-marker-9f4c2b";
const CAPTURE_BODY: Uint8Array<ArrayBuffer> = new TextEncoder().encode(JSON.stringify({
  model: "gpt-5.6-sol",
  stream: false,
  input: MARKER,
}));

const newSyntheticKey = (): Uint8Array<ArrayBuffer> =>
  crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;

const seedAuthenticatedKey = async (kv: Deno.Kv, token: string): Promise<void> => {
  const now = Date.now();
  const hash = await sha256Base64Url(token);
  const id = `request-capture-${hash.slice(0, 12)}`;
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
    name: "sentinel request capture test key",
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

const captureRequest = (token: string | null): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: {
      ...(token === null ? {} : { Authorization: `Bearer ${token}` }),
      "Content-Type": "application/json",
    },
    body: CAPTURE_BODY,
  });

const exportUrl = (params: Record<string, string>): string =>
  `https://ai.ubq.fi/admin/sentinel/replay-captures?${new URLSearchParams(params)}`;

const countManifests = async (kv: Deno.Kv): Promise<number> => {
  let count = 0;
  for await (const _entry of kv.list({ prefix: SENTINEL_REPLAY_MANIFEST_PREFIX })) count += 1;
  return count;
};

type TerminalLogInput = Parameters<typeof withTerminalRequestLog>[1];
type ReplayPersistence = NonNullable<TerminalLogInput["persistSentinelReplay"]>;

const acceptedInput = (): AcceptedSentinelReplayInput => ({
  endpoint: "/v1/responses",
  method: "POST",
  body: new TextEncoder().encode(JSON.stringify({ input: "success-not-persisted" })),
  content_type: "application/json",
  compatibility_headers: {},
  request_id: "request-capture-success",
  git_sha: "fixture-git-sha",
  deno_revision: "fixture-deno-revision",
});

const successResponse = (): Response =>
  new Response(
    JSON.stringify({
      id: "resp_synthetic",
      object: "response",
      status: "completed",
      model: "gpt-5.6-sol",
      output: [{ type: "message", role: "assistant", content: [{ type: "output_text", text: "ok" }] }],
    }),
    { status: 200, headers: { "Content-Type": "application/json" } },
  );

const ignoredTerminalServices = {
  recordTelemetry: () =>
    Promise.resolve({
      status: "ignored" as const,
      reason: "unknown_release" as const,
      release: null,
      provider: null,
      route: null,
      model_hash: null,
    }),
  recordCacheAnalytics: () =>
    Promise.resolve({
      status: "ignored" as const,
      reason: "unknown_release" as const,
      bucket_start_at_ms: null,
    }),
  recordAdminError: () => Promise.resolve(),
};

Deno.test({
  name: "real handler failure capture persists exact accepted bytes through producer encryption and decryption",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    const keyBytes = newSyntheticKey();
    // The stripped child has no real key; a local synthetic key drives the
    // existing SENTINEL_REPLAY_KEY mechanism without ever reading ambient
    // values (it is overwritten before any persistence call).
    Deno.env.set("SENTINEL_REPLAY_KEY", base64UrlEncode(keyBytes));
    const token = `u_${encodeHex(crypto.getRandomValues(new Uint8Array(32)))}`;
    try {
      await seedAuthenticatedKey(kv, token);
      // Deterministic offline failure through the real provider dispatch:
      // the existing debug-routing KV seam forces a Codex failure response
      // without any network, model, or credential dependency.
      await kv.set(DEBUG_ROUTING_KEY, {
        scenario: "codex_429",
        expires_at_ms: null,
        updated_at_ms: Date.now(),
      });

      const response = await handler(captureRequest(token));
      assert.ok(response.status >= 400, `expected a failure response, got ${response.status}`);
      const responseText = await response.text();
      assert.equal(responseText.includes(MARKER), false, "failure response leaks request plaintext");

      const page = await handler(
        new Request(exportUrl({ after_ms: "0", before_ms: String(Date.now() + 1) }), {
          headers: { Authorization: `Bearer ${SUPER_ADMIN_TOKEN}` },
        }),
      );
      assert.equal(page.status, 200);
      const exported = await page.json() as {
        data: ExportedSentinelReplayCapture[];
        cursor: string | null;
      };
      assert.equal(exported.data.length, 1);
      const capture = exported.data[0];
      assert.ok(capture);
      assert.equal(JSON.stringify(exported).includes(MARKER), false, "export leaks request plaintext");

      const plaintext = await decryptExportedSentinelReplay(capture, keyBytes);
      assert.equal(plaintext.endpoint, "/v1/responses");
      assert.equal(plaintext.method, "POST");
      assert.equal(plaintext.content_type, "application/json");
      assert.equal(plaintext.request_id.length > 0, true);
      assert.deepEqual([...plaintext.body], [...CAPTURE_BODY]);
      assert.equal(new TextDecoder().decode(plaintext.body), new TextDecoder().decode(CAPTURE_BODY));
      assert.equal(plaintext.observation.status, response.status);
      assert.equal(plaintext.client_observation.terminal_type, "http.error");
    } finally {
      Deno.env.delete("SENTINEL_REPLAY_KEY");
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      await kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test({
  name: "unauthorized requests are never captured",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    try {
      const anonymous = await handler(captureRequest(null));
      assert.equal(anonymous.status, 401);
      assert.equal(await countManifests(kv), 0);

      const unknownKey = `u_${encodeHex(crypto.getRandomValues(new Uint8Array(32)))}`;
      const invalid = await handler(captureRequest(unknownKey));
      assert.equal(invalid.status, 401);
      assert.equal(await countManifests(kv), 0);
    } finally {
      await kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test("successful terminal responses are never persisted and release captured bytes", async () => {
  const input = acceptedInput();
  const originalBytes = [...input.body];
  let persistenceCalls = 0;
  const persistSentinelReplay: ReplayPersistence = () => {
    persistenceCalls += 1;
    return Promise.resolve({ status: "disabled" as const, reason: "kv_unavailable" });
  };
  try {
    const response = await withTerminalRequestLog(successResponse(), {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "request-capture-success",
      sentinelReplayInput: input,
      persistSentinelReplay,
      ...ignoredTerminalServices,
    });
    assert.equal(response.status, 200);
    assert.equal(persistenceCalls, 0, "a successful response must never be persisted");
    assert.ok(input.body.every((byte) => byte === 0), "captured bytes must be zeroed after terminal handling");
    assert.deepEqual([...new Uint8Array(originalBytes.length)], originalBytes.map(() => 0));
  } finally {
    input.body.fill(0);
  }
});
