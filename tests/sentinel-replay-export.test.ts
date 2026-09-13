import assert from "node:assert/strict";
import { config } from "../src/config.ts";
import { setKvForTest } from "../src/kv.ts";
import {
  type AcceptedSentinelReplayInput,
  decryptExportedSentinelReplay,
  type ExportedSentinelReplayCapture,
  persistEncryptedSentinelReplay,
  type SentinelFailureObservation,
} from "../src/sentinel_replay_capture.ts";
import { handleAdminSentinelReplayCaptures } from "../src/sentinel_replay_admin.ts";
import { PASSKEY_RELAY_COOKIE_NAME, passkeyHandleKey, passkeySessionKey, passkeyUserKey } from "../src/passkeys.ts";
import { linkSentinelReplayToIncident } from "../src/sentinel_incident_outbox.ts";

const { default: handler } = await import("../src/handler.ts");

const SUPER_ADMIN_TOKEN = "sentinel-replay-export-super-admin-token";
const INCIDENT_ID = "provider-12345678-1234-4abc-8def-1234567890ab";
const kvAvailable = typeof Deno.openKv === "function";

const encoder = new TextEncoder();

const failureObservation = (): SentinelFailureObservation => ({
  status: 502,
  stream: false,
  completed: false,
  terminal_type: "http.error",
  failure_kind: "upstream_timeout",
  synthetic_terminal_type: null,
  provider_route: "test-provider",
});

const syntheticInput = (bytes: Uint8Array<ArrayBuffer>, requestId: string): AcceptedSentinelReplayInput => ({
  endpoint: "/v1/responses",
  method: "POST",
  body: bytes,
  content_type: "application/json",
  compatibility_headers: {},
  request_id: requestId,
  git_sha: "synthetic-git-sha",
  deno_revision: "synthetic-revision",
});

const twelveByteIv = (): Uint8Array<ArrayBuffer> => new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);

const expectStored = (result: Awaited<ReturnType<typeof persistEncryptedSentinelReplay>>) => {
  assert.equal(result.status, "stored");
  if (result.status !== "stored") throw new Error("expected a stored replay capture");
  return result;
};

const superAdminHeaders = { Authorization: `Bearer ${SUPER_ADMIN_TOKEN}` };

const exportUrl = (params: Record<string, string>): string => `https://ai.ubq.fi/admin/sentinel/replay-captures?${new URLSearchParams(params).toString()}`;

/**
 * Code-unit ascending string order: exactly what an argument-less
 * `Array.prototype.sort()` does for strings, stated explicitly so these
 * order-insensitive comparisons keep their byte-identical ordering.
 */
const compareStrings = (left: string, right: string): number => {
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
};

/** The first exported capture, which every one of these assertions expects. */
const firstCapture = (body: { data: ExportedSentinelReplayCapture[] }): ExportedSentinelReplayCapture => {
  const capture = body.data[0];
  assert.ok(capture, "the export must return at least one capture");
  return capture;
};

/** Simulates a runtime with no Deno KV available, without touching the filesystem. */
const runWithoutKv = async <T>(fn: () => Promise<T>): Promise<T> => {
  const descriptor = Object.getOwnPropertyDescriptor(Deno, "openKv");
  try {
    Object.defineProperty(Deno, "openKv", { value: undefined, configurable: true });
    setKvForTest(null);
    return await fn();
  } finally {
    if (descriptor) Object.defineProperty(Deno, "openKv", descriptor);
    setKvForTest(null);
  }
};

Deno.test("anonymous replay-captures requests are rejected before any storage access", async () => {
  await runWithoutKv(async () => {
    const anonymous = await handler(new Request(exportUrl({ after_ms: "0", before_ms: "1" })));
    assert.equal(anonymous.status, 401);
    const anonymousPayload = (await anonymous.json()) as { error?: { code?: string } };
    assert.equal(anonymousPayload.error?.code, "invalid_api_key");

    // The same request with a valid super-admin token must reach storage, and
    // missing storage must fail closed rather than return an empty success.
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    try {
      const authenticated = await handler(new Request(exportUrl({ after_ms: "0", before_ms: "1" }), { headers: superAdminHeaders }));
      assert.equal(authenticated.status, 503);
      const payload = (await authenticated.json()) as { error?: { code?: string } };
      assert.equal(payload.error?.code, "sentinel_replay_storage_unavailable");
    } finally {
      adminTokens.delete(SUPER_ADMIN_TOKEN);
    }
  });
});

Deno.test({
  name: "an authenticated non-super-admin is rejected before the replay export",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const now = Date.now();
    const userId = "sentinel-replay-passkey-user";
    const handle = "sentinel-replay-passkey-handle";
    const sessionToken = "sentinel-replay-passkey-session-token";
    await kv.set(passkeyUserKey(userId), {
      id: userId,
      handle,
      is_admin: true,
      credential_ids: ["sentinel-replay-credential"],
      created_at_ms: now,
      updated_at_ms: now,
    });
    await kv.set(passkeyHandleKey(handle), userId);
    await kv.set(passkeySessionKey(sessionToken), {
      token: sessionToken,
      user_id: userId,
      created_at_ms: now,
      expires_at_ms: now + 3_600_000,
    });
    try {
      const response = await handler(
        new Request(exportUrl({ after_ms: "0", before_ms: "1" }), {
          headers: { Cookie: `${PASSKEY_RELAY_COOKIE_NAME}=${encodeURIComponent(sessionToken)}` },
        })
      );
      assert.equal(response.status, 403);
      const payload = (await response.json()) as { error?: { message?: string; code?: string } };
      assert.equal(payload.error?.message, "Super admin token required");
      assert.equal(payload.error?.code, "forbidden");
    } finally {
      kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test({
  name: "replay-captures export returns encrypted captures that decrypt to exact synthetic bytes",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    try {
      const keyBytes = crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;
      const nowMs = 1_700_000_000_000;
      const exactBytes: Uint8Array<ArrayBuffer> = encoder.encode(
        JSON.stringify({
          model: "gpt-5.6-sol",
          stream: false,
          messages: [{ role: "user", content: "synthetic replay bytes for export round trip" }],
        })
      );
      const input = syntheticInput(exactBytes, "synthetic-export-request-id");
      const stored = expectStored(
        await persistEncryptedSentinelReplay(input, failureObservation(), {
          kv,
          keyBytes,
          now: () => nowMs,
          randomUuid: () => "synthetic-capture-1",
          randomBytes: twelveByteIv,
        })
      );

      const response = await handler(new Request(exportUrl({ after_ms: "0", before_ms: String(nowMs + 1) }), { headers: superAdminHeaders }));
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("Cache-Control"), "no-store");
      const body = (await response.json()) as {
        data: ExportedSentinelReplayCapture[];
        cursor: string | null;
      };
      assert.equal(body.data.length, 1);
      assert.equal(body.data[0]?.manifest.fingerprint, stored.manifest.fingerprint);
      assert.equal(body.data[0]?.manifest.algorithm, "AES-256-GCM");
      assert.equal(body.data[0]?.manifest.compression, "gzip");
      // The exported payload is encrypted only: no plaintext bytes may appear.
      assert.equal(body.data[0]?.chunks.join("").includes("synthetic replay bytes for export round trip"), false);

      const plaintext = await decryptExportedSentinelReplay(firstCapture(body), keyBytes);
      assert.equal(plaintext.endpoint, input.endpoint);
      assert.equal(plaintext.method, input.method);
      assert.equal(plaintext.content_type, input.content_type);
      assert.equal(plaintext.request_id, input.request_id);
      assert.equal(plaintext.git_sha, input.git_sha);
      assert.equal(plaintext.deno_revision, input.deno_revision);
      assert.deepEqual([...plaintext.body], [...exactBytes]);
      assert.equal(new TextDecoder().decode(plaintext.body), new TextDecoder().decode(exactBytes));
    } finally {
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test({
  name: "replay-captures export paginates multiple captures with an exhaustible cursor",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    try {
      const keyBytes = crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;
      const nowMs = 1_600_000_000_000;
      const requestIds: string[] = [];
      for (let index = 1; index <= 3; index += 1) {
        const requestId = `synthetic-pagination-request-${index}`;
        requestIds.push(requestId);
        const bytes: Uint8Array<ArrayBuffer> = encoder.encode(
          JSON.stringify({
            model: "gpt-5.6-sol",
            seq: index,
            content: `synthetic pagination capture ${index}`,
          })
        );
        const result = expectStored(
          await persistEncryptedSentinelReplay(syntheticInput(bytes, requestId), failureObservation(), {
            kv,
            keyBytes,
            now: () => nowMs + index * 1_000,
            randomUuid: () => `synthetic-pagination-capture-${index}`,
            randomBytes: twelveByteIv,
          })
        );
        assert.equal(result.manifest.capture_id, `synthetic-pagination-capture-${index}`, requestId);
      }

      const collected: ExportedSentinelReplayCapture[] = [];
      const pageCapturedAt: number[] = [];
      let cursor: string | null = null;
      let pages = 0;
      for (;;) {
        const params: Record<string, string> = {
          after_ms: "0",
          before_ms: String(nowMs + 10_000),
        };
        if (cursor !== null) params.cursor = cursor;
        const response = await handler(new Request(exportUrl(params), { headers: superAdminHeaders }));
        assert.equal(response.status, 200);
        const body = (await response.json()) as {
          data: ExportedSentinelReplayCapture[];
          cursor: string | null;
        };
        for (const capture of body.data) {
          collected.push(capture);
          pageCapturedAt.push(capture.manifest.captured_at_ms);
        }
        cursor = body.cursor;
        pages += 1;
        assert.ok(pages <= 10, "pagination must terminate");
        if (cursor === null) break;
      }

      assert.equal(collected.length, 3);
      assert.equal(new Set(collected.map((capture) => capture.manifest.fingerprint)).size, 3);
      assert.deepEqual(
        pageCapturedAt,
        [...pageCapturedAt].sort((left, right) => left - right)
      );
      const decryptedRequestIds = new Set<string>();
      for (const capture of collected) {
        const plaintext = await decryptExportedSentinelReplay(capture, keyBytes);
        decryptedRequestIds.add(plaintext.request_id);
      }
      assert.deepEqual([...decryptedRequestIds].sort(compareStrings), [...requestIds].sort(compareStrings));
    } finally {
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test({
  name: "replay-captures export scopes to a linked incident and stays encrypted",
  ignore: !kvAvailable,
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    setKvForTest(kv);
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    try {
      const keyBytes = crypto.getRandomValues(new Uint8Array(32)).slice() as Uint8Array<ArrayBuffer>;
      const nowMs = 1_500_000_000_000;
      const bytes: Uint8Array<ArrayBuffer> = encoder.encode(
        JSON.stringify({
          model: "gpt-5.6-sol",
          content: "incident-linked synthetic capture",
        })
      );
      const stored = expectStored(
        await persistEncryptedSentinelReplay(syntheticInput(bytes, "synthetic-incident-request"), failureObservation(), {
          kv,
          keyBytes,
          now: () => nowMs,
          randomUuid: () => "synthetic-incident-capture",
          randomBytes: twelveByteIv,
        })
      );
      const manifestKey = stored.manifest_key;
      assert.ok(manifestKey, "a stored capture must expose its manifest key");
      await linkSentinelReplayToIncident(kv, INCIDENT_ID, stored.manifest.fingerprint, manifestKey);

      const response = await handler(
        new Request(
          exportUrl({
            after_ms: "0",
            before_ms: String(nowMs + 1),
            incident_id: INCIDENT_ID,
          }),
          { headers: superAdminHeaders }
        )
      );
      assert.equal(response.status, 200);
      const body = (await response.json()) as {
        data: ExportedSentinelReplayCapture[];
        cursor: string | null;
      };
      assert.equal(body.data.length, 1);
      assert.equal(body.data[0]?.manifest.fingerprint, stored.manifest.fingerprint);
      const plaintext = await decryptExportedSentinelReplay(firstCapture(body), keyBytes);
      assert.equal(plaintext.request_id, "synthetic-incident-request");
      assert.deepEqual([...plaintext.body], [...bytes]);

      // An incident listing for a foreign incident ID yields an empty page,
      // never another incident's capture.
      const foreign = await handler(
        new Request(
          exportUrl({
            after_ms: "0",
            before_ms: String(nowMs + 1),
            incident_id: "provider-00000000-0000-4000-8000-000000000000",
          }),
          { headers: superAdminHeaders }
        )
      );
      assert.equal(foreign.status, 200);
      const foreignBody = (await foreign.json()) as { data: unknown[]; cursor: string | null };
      assert.deepEqual(foreignBody.data, []);
    } finally {
      adminTokens.delete(SUPER_ADMIN_TOKEN);
      kv.close();
      setKvForTest(null);
    }
  },
});

Deno.test("replay-captures export rejects malformed interval, limit, cursor, and incident parameters", async () => {
  await runWithoutKv(async () => {
    const adminTokens = config.adminTokens as Set<string>;
    adminTokens.add(SUPER_ADMIN_TOKEN);
    try {
      for (const query of [
        "after_ms=abc&before_ms=1",
        "after_ms=10&before_ms=5",
        "after_ms=0",
        "after_ms=0&before_ms=1&limit=0",
        "after_ms=0&before_ms=1&limit=2",
        "after_ms=0&before_ms=1&limit=abc",
        "after_ms=0&before_ms=1&cursor=!!",
        `after_ms=0&before_ms=1&cursor=${"a".repeat(3_000)}`,
        "after_ms=0&before_ms=1&incident_id=not-an-incident-id",
      ]) {
        const response = await handler(new Request(`https://ai.ubq.fi/admin/sentinel/replay-captures?${query}`, { headers: superAdminHeaders }));
        assert.equal(response.status, 400, query);
      }
    } finally {
      adminTokens.delete(SUPER_ADMIN_TOKEN);
    }
  });
});

Deno.test("replay-captures export fails closed when the storage read fails instead of succeeding empty", async () => {
  const request = new Request(exportUrl({ after_ms: "0", before_ms: "1" }));
  const brokenKv = { list: null } as unknown as Deno.Kv;
  const interval = await handleAdminSentinelReplayCaptures(request, {
    getKv: () => Promise.resolve(brokenKv),
    listEncryptedSentinelReplays: () => {
      throw new Error("injected listing failure");
    },
  });
  assert.equal(interval.status, 503);
  const intervalPayload = (await interval.json()) as { error?: { code?: string } };
  assert.equal(intervalPayload.error?.code, "sentinel_replay_export_failed");

  const incidentRequest = new Request(exportUrl({ after_ms: "0", before_ms: "1", incident_id: INCIDENT_ID }));
  const incident = await handleAdminSentinelReplayCaptures(incidentRequest, {
    getKv: () => Promise.resolve(brokenKv),
    listEncryptedSentinelIncidentReplays: () => {
      throw new Error("injected incident listing failure");
    },
  });
  assert.equal(incident.status, 503);
  const incidentPayload = (await incident.json()) as { error?: { code?: string } };
  assert.equal(incidentPayload.error?.code, "sentinel_replay_export_failed");
});
