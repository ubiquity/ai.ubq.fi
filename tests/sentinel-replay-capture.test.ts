import assert from "node:assert/strict";
import {
  type AcceptedSentinelReplayInput,
  captureAcceptedSentinelReplayInput,
  createSentinelSseInspector,
  decodeSentinelReplayKey,
  decryptExportedSentinelReplay,
  discardSentinelReplayCaptureCandidate,
  inspectSentinelBufferedResponse,
  inspectSentinelBufferedResponseBody,
  isExportedSentinelReplayCapture,
  listEncryptedSentinelIncidentReplays,
  listEncryptedSentinelReplays,
  materializeSentinelReplayInput,
  normalizeSentinelCompatibilityHeaders,
  persistEncryptedSentinelReplay,
  resolveSentinelClientFailureObservation,
  SENTINEL_REPLAY_CHUNK_BYTES,
  SENTINEL_REPLAY_CHUNK_PREFIX,
  SENTINEL_REPLAY_EXPORT_PAGE_LIMIT,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
  SENTINEL_REPLAY_MAX_BODY_BYTES,
  SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES,
  SENTINEL_REPLAY_TTL_MS,
  type SentinelClientFailureObservation,
  type SentinelFailureObservation,
  sentinelFailureSignature,
  shouldPersistSentinelReplay,
  shouldSignalSentinelIncident,
  zeroSentinelReplayInput,
} from "../src/sentinel_replay_capture.ts";
import {
  coalesceSentinelIncidentFailureEvents,
  createSentinelIncidentFailureEvent,
  type SentinelIncidentFailureEvent,
} from "../src/sentinel_incident_outbox.ts";
import { handleAdminSentinelReplayCaptures } from "../src/sentinel_replay_admin.ts";
import { shouldSignalSentinelProviderDegradation, withTerminalRequestLog } from "../src/handler.ts";
import { captureRawBodyOnce, MAX_ACCEPTED_JSON_BODY_BYTES, observeRawBodyOnce, readJsonBody } from "../src/request.ts";
import { base64UrlDecode, base64UrlEncode } from "../src/utils.ts";
import {
  fetchEncryptedReplayCaptures,
  replayOneCase,
  SENTINEL_MAX_ENCRYPTED_REPLAY_PAGE_BYTES,
  SENTINEL_MAX_REPLAY_EXPORT_PAGES,
  SENTINEL_MAX_REPLAY_RESPONSE_BYTES,
} from "../scripts/sentinel/replay.ts";
import type { ReplayCase } from "../scripts/sentinel/types.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const keyBytes = new Uint8Array(Array.from({ length: 32 }, (_, index) => index + 1));

const acceptedInput = (
  body: Uint8Array = new TextEncoder().encode('{"model":"gpt-5.6-sol"}'),
): AcceptedSentinelReplayInput => ({
  endpoint: "/v1/responses?trace=one",
  method: "POST",
  body: new Uint8Array(body),
  content_type: "application/json; charset=utf-8",
  compatibility_headers: {
    accept: "text/event-stream",
    originator: "codex_cli_rs",
    "user-agent": "codex_cli_rs/0.149.0",
  },
  request_id: "request-one",
  git_sha: "a".repeat(40),
  deno_revision: "revision-one",
});

const failedObservation = (overrides: Partial<SentinelFailureObservation> = {}): SentinelFailureObservation => ({
  status: 502,
  stream: false,
  completed: false,
  terminal_type: "error",
  failure_kind: "read_error",
  synthetic_terminal_type: null,
  provider_route: "chatgpt_codex",
  ...overrides,
});

const failedClientObservation = (
  overrides: Partial<SentinelClientFailureObservation> = {},
): SentinelClientFailureObservation => ({
  status: 502,
  stream: false,
  completed: false,
  terminal_type: "error",
  failure_kind: "read_error",
  framing_valid: true,
  provider_route: "chatgpt_codex",
  ...overrides,
});

Deno.test("sentinel replay captures exact accepted bytes while excluding credentials", async () => {
  const exactBody = new Uint8Array([32, 123, 34, 120, 34, 58, 49, 125, 10]);
  const request = new Request("https://ai.ubq.fi/v1/responses?trace=one", {
    method: "POST",
    headers: {
      Accept: "text/event-stream",
      Authorization: "Bearer must-not-be-captured",
      Cookie: "session=must-not-be-captured",
      "Content-Type": "application/json",
      Originator: "codex_cli_rs",
      "OpenAI-Beta": "responses=v1",
    },
    body: exactBody,
  });
  const candidate = captureAcceptedSentinelReplayInput(request, "request-one");
  assert.ok(candidate);
  assert.equal(materializeSentinelReplayInput(candidate), null);
  assert.deepEqual(await readJsonBody(request), { x: 1 });
  const captured = materializeSentinelReplayInput(candidate);
  assert.ok(captured);
  assert.equal(candidate.body, null);
  assert.deepEqual(captured.body, exactBody);
  assert.equal(captured.endpoint, "/v1/responses?trace=one");
  assert.equal(captured.compatibility_headers.authorization, undefined);
  assert.equal(captured.compatibility_headers.cookie, undefined);
  assert.equal(captured.compatibility_headers.originator, "codex_cli_rs");
  await assert.rejects(
    () =>
      persistEncryptedSentinelReplay(
        {
          ...captured,
          compatibility_headers: { ...captured.compatibility_headers, authorization: "Bearer must-not-persist" },
        },
        failedObservation(),
        { kv: new CountingKv() as unknown as Deno.Kv, keyBytes },
      ),
    /disallowed value/,
  );
  zeroSentinelReplayInput(captured);
  assert.deepEqual(new Set(captured.body), new Set([0]));
});

Deno.test("request reader and replay capture share the 32 MiB accepted-body limit", async () => {
  assert.equal(SENTINEL_REPLAY_MAX_BODY_BYTES, MAX_ACCEPTED_JSON_BODY_BYTES);
  let cancelled = false;
  const request = new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Content-Length": String(MAX_ACCEPTED_JSON_BODY_BYTES + 1),
    },
    body: new ReadableStream<Uint8Array>({
      pull(controller) {
        controller.enqueue(new TextEncoder().encode("{}"));
      },
      cancel() {
        cancelled = true;
      },
    }),
  });
  const candidate = captureAcceptedSentinelReplayInput(request, "oversized-request");
  assert.ok(candidate);
  assert.equal(await readJsonBody(request), null);
  assert.equal(cancelled, true);
  assert.equal(materializeSentinelReplayInput(candidate), null);

  const accepted = new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: '{"prompt":"sensitive"}',
  });
  const acceptedCandidate = captureAcceptedSentinelReplayInput(accepted, "discarded-request");
  assert.ok(acceptedCandidate);
  assert.deepEqual(await readJsonBody(accepted), { prompt: "sensitive" });
  const retainedBytes = acceptedCandidate.body;
  assert.ok(retainedBytes);
  discardSentinelReplayCaptureCandidate(acceptedCandidate);
  assert.equal(acceptedCandidate.body, null);
  assert.deepEqual(new Set(retainedBytes), new Set([0]));
});

Deno.test("raw body replay capture rejects over-cap input and remains one-shot", () => {
  const request = new Request("https://ai.ubq.fi/v1/images/edits");
  let observations = 0;
  observeRawBodyOnce(request, () => {
    observations += 1;
  });
  const oversized = new Uint8Array(MAX_ACCEPTED_JSON_BODY_BYTES + 1);
  assert.equal(captureRawBodyOnce(request, oversized), false);
  assert.equal(captureRawBodyOnce(request, new Uint8Array([1])), false);
  assert.equal(observations, 0);
  oversized.fill(0);
});

Deno.test("sentinel compatibility headers normalize only the replay allowlist", () => {
  const normalized = normalizeSentinelCompatibilityHeaders(
    new Headers({
      Accept: " application/json ",
      Authorization: "Bearer secret",
      Cookie: "secret",
      "OpenAI-Project": "project",
      "X-Unrelated": "ignored",
      "X-Stainless-Runtime": "deno",
    }),
  );
  assert.deepEqual(normalized, {
    accept: "application/json",
    "openai-project": "project",
    "x-stainless-runtime": "deno",
  });
});

Deno.test("sentinel replay key accepts one base64url encoded AES-256 key", () => {
  const encoded = base64UrlEncode(keyBytes);
  assert.deepEqual(decodeSentinelReplayKey(encoded), keyBytes);
  assert.deepEqual(decodeSentinelReplayKey(`${encoded}=`), keyBytes);
  assert.equal(decodeSentinelReplayKey(base64UrlEncode(new Uint8Array(31))), null);
  assert.equal(decodeSentinelReplayKey(` ${encoded}`), null);
  assert.equal(decodeSentinelReplayKey("not base64"), null);
});

Deno.test("sentinel failure classifier excludes success and client cancellation", () => {
  assert.equal(shouldPersistSentinelReplay(failedObservation()), true);
  assert.equal(shouldPersistSentinelReplay(failedObservation({ status: 504, terminal_type: "deadline" })), true);
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      terminal_type: "response.failed",
      failure_kind: null,
    })),
    true,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      completed: false,
      terminal_type: "response.incomplete",
      failure_kind: null,
    })),
    false,
  );
  const cancelled = failedObservation({
    status: 499,
    stream: true,
    terminal_type: "cancelled",
    failure_kind: "request_cancelled",
  });
  assert.equal(
    shouldPersistSentinelReplay(
      cancelled,
      failedClientObservation({
        status: 499,
        stream: false,
        terminal_type: "http.error",
        failure_kind: "request_cancelled",
      }),
    ),
    false,
  );
  assert.equal(
    shouldPersistSentinelReplay(
      cancelled,
      failedClientObservation({
        status: 499,
        stream: true,
        terminal_type: null,
        failure_kind: "missing_sse_terminal",
        framing_valid: false,
      }),
    ),
    false,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      completed: false,
      terminal_type: "response.incomplete",
      failure_kind: "max_output_tokens",
    })),
    false,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      completed: false,
      terminal_type: "response.incomplete",
      failure_kind: "response_incomplete:provider_internal_deadline",
    })),
    true,
  );
  assert.equal(
    shouldPersistSentinelReplay(
      failedObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: null,
      }),
      failedClientObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: null,
        framing_valid: false,
      }),
    ),
    true,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      terminal_type: null,
      failure_kind: null,
      synthetic_terminal_type: "response.failed",
    })),
    true,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 499,
      stream: true,
      terminal_type: "cancelled",
      failure_kind: "stale_primary_failure",
    })),
    false,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      completed: false,
      terminal_type: "cancelled",
      failure_kind: null,
    })),
    false,
  );
  assert.equal(
    shouldPersistSentinelReplay(failedObservation({
      status: 200,
      stream: true,
      completed: true,
      terminal_type: "response.completed",
      failure_kind: "stale_primary_failure",
    })),
    false,
  );
});

Deno.test("sentinel incident signals require trusted provider or gateway evidence", () => {
  for (const status of [400, 401, 404, 429]) {
    const internal = failedObservation({
      status,
      stream: false,
      terminal_type: "http.error",
      failure_kind: null,
    });
    assert.equal(
      shouldSignalSentinelIncident(
        internal,
        failedClientObservation({
          status,
          stream: false,
          terminal_type: "http.error",
          failure_kind: null,
        }),
      ),
      false,
    );
    assert.equal(shouldPersistSentinelReplay(internal), true);
  }

  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 200, stream: true, terminal_type: "response.failed", failure_kind: null }),
      failedClientObservation({
        status: 200,
        stream: true,
        terminal_type: "response.failed",
        failure_kind: null,
      }),
    ),
    false,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 200, stream: true, terminal_type: "response.failed", failure_kind: null }),
      failedClientObservation({
        status: 200,
        stream: true,
        terminal_type: "response.failed",
        failure_kind: "server_error",
      }),
    ),
    true,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 400, stream: false, terminal_type: "http.error", failure_kind: null }),
      failedClientObservation({
        status: 400,
        stream: false,
        terminal_type: "http.error",
        failure_kind: "invalid_request_error",
      }),
    ),
    false,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 500, terminal_type: "http.error", failure_kind: null }),
      failedClientObservation({ status: 500, terminal_type: "http.error", failure_kind: null }),
    ),
    true,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 200, stream: true, terminal_type: "error", failure_kind: "read_error" }),
      failedClientObservation({ status: 200, stream: true, terminal_type: "error", failure_kind: "read_error" }),
    ),
    true,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: null,
      }),
      failedClientObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: null,
        framing_valid: false,
      }),
    ),
    true,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({
        status: 200,
        stream: true,
        completed: false,
        terminal_type: "response.incomplete",
        failure_kind: "max_output_tokens",
      }),
      failedClientObservation({
        status: 200,
        stream: true,
        completed: false,
        terminal_type: "response.incomplete",
        failure_kind: "max_output_tokens",
      }),
    ),
    false,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({
        status: 200,
        stream: true,
        completed: false,
        terminal_type: "response.incomplete",
        failure_kind: "response_incomplete:provider_internal_deadline",
      }),
      failedClientObservation({
        status: 200,
        stream: true,
        completed: false,
        terminal_type: "response.incomplete",
        failure_kind: "response_incomplete:provider_internal_deadline",
      }),
    ),
    true,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: "stale_primary_failure",
      }),
      failedClientObservation({
        status: 200,
        stream: true,
        completed: true,
        terminal_type: "response.completed",
        failure_kind: "stale_primary_failure",
      }),
    ),
    false,
  );
  assert.equal(
    shouldSignalSentinelIncident(
      failedObservation({ status: 499, stream: true, terminal_type: "cancelled", failure_kind: "request_cancelled" }),
      failedClientObservation({
        status: 499,
        stream: true,
        terminal_type: "cancelled",
        failure_kind: "request_cancelled",
        framing_valid: false,
      }),
    ),
    false,
  );
});

Deno.test("masked provider degradation signals an incident without capturing a successful request", () => {
  assert.equal(
    shouldSignalSentinelProviderDegradation({
      status: 200,
      completed: true,
      removedProviderTriggerClass: "read_error",
    }),
    true,
  );
  for (
    const input of [
      { status: 502, completed: false, removedProviderTriggerClass: "read_error" },
      { status: 200, completed: false, removedProviderTriggerClass: "read_error" },
      { status: 200, completed: true, removedProviderTriggerClass: null },
    ]
  ) {
    assert.equal(shouldSignalSentinelProviderDegradation(input), false);
  }
});

Deno.test("sentinel encrypted persistence rejects a cancelled request even with stale failure state", async () => {
  await assert.rejects(
    () =>
      persistEncryptedSentinelReplay(
        acceptedInput(),
        failedObservation({
          status: 499,
          stream: true,
          terminal_type: "cancelled",
          failure_kind: "stale_primary_failure",
          synthetic_terminal_type: "response.failed",
        }),
        { kv: new CountingKv() as unknown as Deno.Kv, keyBytes },
      ),
    /successful request cannot be persisted/,
  );
});

Deno.test("sentinel replay applies the exact 48-hour TTL to chunks, dedupe, and manifest keys", async () => {
  const backing = new CountingKv();
  const expirations: number[] = [];
  const kv = {
    get: backing.get.bind(backing),
    getMany: backing.getMany.bind(backing),
    list: backing.list.bind(backing),
    set(key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) {
      expirations.push(options?.expireIn ?? -1);
      return backing.set(key, value, options);
    },
    atomic() {
      const original = backing.atomic();
      const wrapped = {
        check(entry: Deno.KvEntryMaybe<unknown>) {
          original.check(entry);
          return wrapped;
        },
        set(key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) {
          expirations.push(options?.expireIn ?? -1);
          original.set(key, value, options);
          return wrapped;
        },
        commit: () => original.commit(),
      };
      return wrapped;
    },
  };
  const result = await persistEncryptedSentinelReplay(
    acceptedInput(new Uint8Array(150_000).fill(31)),
    failedObservation(),
    {
      kv: kv as unknown as Deno.Kv,
      keyBytes,
      now: () => 1_777_000_000_000,
    },
  );
  assert.equal(result.status, "stored");
  assert.ok(expirations.length >= 3);
  assert.deepEqual(new Set(expirations), new Set([SENTINEL_REPLAY_TTL_MS]));
});

Deno.test("sentinel keyed digests are stable under header order and sensitive to body, endpoint, headers, and failure", async () => {
  const persist = async (input: AcceptedSentinelReplayInput, observation = failedObservation()) => {
    const result = await persistEncryptedSentinelReplay(input, observation, {
      kv: new CountingKv() as unknown as Deno.Kv,
      keyBytes,
      now: () => 1_777_000_000_000,
      randomBytes: () => new Uint8Array(12),
    });
    assert.equal(result.status, "stored");
    if (result.status !== "stored") throw new Error("fixture failed");
    return result.manifest;
  };
  const base = acceptedInput();
  const reordered = await persist({
    ...base,
    compatibility_headers: {
      "user-agent": base.compatibility_headers["user-agent"]!,
      accept: base.compatibility_headers.accept!,
      originator: base.compatibility_headers.originator!,
    },
  });
  const original = await persist(base);
  assert.equal(reordered.fingerprint, original.fingerprint);
  const differentBody = await persist({ ...base, body: new TextEncoder().encode("different") });
  assert.notEqual(differentBody.fingerprint, original.fingerprint);
  assert.notEqual(differentBody.case_group_digest, original.case_group_digest);
  assert.notEqual((await persist({ ...base, endpoint: "/v1/chat/completions" })).fingerprint, original.fingerprint);
  assert.notEqual(
    (await persist({ ...base, compatibility_headers: { accept: "application/json" } })).fingerprint,
    original.fingerprint,
  );
  const differentFailure = await persist(base, failedObservation({ failure_kind: "inactivity_timeout" }));
  assert.notEqual(differentFailure.fingerprint, original.fingerprint);
  assert.equal(differentFailure.case_group_digest, original.case_group_digest);
});

Deno.test("sentinel concurrent capture publication deduplicates and rejects excessive assembly metadata", async () => {
  const kv = new CountingKv();
  const results = await Promise.all([
    persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
      kv: kv as unknown as Deno.Kv,
      keyBytes,
      now: () => 1_777_000_000_000,
      randomUuid: () => "capture-concurrent-one",
    }),
    persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
      kv: kv as unknown as Deno.Kv,
      keyBytes,
      now: () => 1_777_000_000_000,
      randomUuid: () => "capture-concurrent-two",
    }),
  ]);
  assert.deepEqual(results.map((result) => result.status).sort(), ["duplicate", "stored"]);
  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  });
  assert.equal(page.captures.length, 1);
  const retainedChunks = [...kv.entries.values()].filter((entry) =>
    SENTINEL_REPLAY_CHUNK_PREFIX.every((part, index) => Object.is(part, entry.key[index]))
  );
  assert.equal(retainedChunks.length, page.captures[0]!.manifest.chunk_count);
  await assert.rejects(
    () =>
      decryptExportedSentinelReplay({
        ...page.captures[0]!,
        manifest: { ...page.captures[0]!.manifest, chunk_count: 4_097 },
      }, keyBytes),
    /export is invalid/,
  );
});

Deno.test("sentinel replay encrypts, chunks, exports, decrypts, and deduplicates exact bytes", async () => {
  const kv = new CountingKv();
  const body = new Uint8Array(SENTINEL_REPLAY_CHUNK_BYTES * 4);
  let state = 0x9e3779b9;
  for (let index = 0; index < body.length; index += 1) {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    body[index] = state & 0xff;
  }
  const input = acceptedInput(body);
  const now = 1_777_000_000_000;
  const first = await persistEncryptedSentinelReplay(input, failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => now,
    randomUuid: () => "capture-one",
    randomBytes: () => new Uint8Array(12).fill(7),
  });
  assert.equal(first.status, "stored");
  if (first.status !== "stored") return;
  assert.equal(first.manifest.expires_at_ms - first.manifest.captured_at_ms, SENTINEL_REPLAY_TTL_MS);
  assert.ok(first.manifest.chunk_count > 1);
  assert.notEqual(JSON.stringify([...kv.entries.values()]).includes("gpt-5.6-sol"), true);

  const duplicate = await persistEncryptedSentinelReplay(input, failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => now + 1,
  });
  assert.equal(duplicate.status, "duplicate");

  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: now - 1,
    beforeMs: now,
  });
  assert.equal(page.captures.length, 1);
  const decrypted = await decryptExportedSentinelReplay(page.captures[0]!, keyBytes);
  assert.deepEqual(decrypted.body, body);
  assert.equal(decrypted.endpoint, input.endpoint);
  assert.equal(decrypted.failure_signature, sentinelFailureSignature(failedClientObservation()));
  assert.deepEqual(decrypted.client_observation, failedClientObservation());
  assert.equal(decrypted.observation.provider_route, "chatgpt_codex");

  await assert.rejects(
    () => decryptExportedSentinelReplay(page.captures[0]!, new Uint8Array(32).fill(99)),
    /operation-specific reason|decrypt|key|ciphertext/i,
  );
  const tampered = {
    ...page.captures[0]!,
    manifest: { ...page.captures[0]!.manifest, case_group_digest: "0".repeat(64) },
  };
  await assert.rejects(
    () => decryptExportedSentinelReplay(tampered, keyBytes),
    /integrity check failed/,
  );
});

Deno.test("durable incidents retain references to exact encrypted replay captures", async () => {
  const kv = new CountingKv();
  const now = 1_777_000_000_000;
  const incidentEvent = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, now, {
    randomUuid: () => "00000000-0000-4000-8000-000000000001",
  });
  const persisted = await persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => now,
    randomUuid: () => "incident-capture",
    incidentEvent,
  });
  assert.equal(persisted.status, "stored");
  if (persisted.status !== "stored" || !persisted.manifest_key) throw new Error("capture fixture failed");
  const readyEvent = (await kv.get<SentinelIncidentFailureEvent>(incidentEvent.key)).value;
  assert.equal(readyEvent?.state, "ready");
  assert.equal(readyEvent?.manifest_key?.join("/"), persisted.manifest_key.join("/"));
  assert.equal(
    await coalesceSentinelIncidentFailureEvents(kv as unknown as Deno.Kv, now + 1, {
      randomAckNonce: () => "A".repeat(43),
    }),
    1,
  );
  const page = await listEncryptedSentinelIncidentReplays(kv as unknown as Deno.Kv, {
    incidentId: incidentEvent.value.incident_id,
  });
  assert.equal(page.captures.length, 1);
  const plaintext = await decryptExportedSentinelReplay(page.captures[0]!, keyBytes);
  assert.deepEqual(plaintext.body, acceptedInput().body);
  plaintext.body.fill(0);
});

Deno.test("sentinel replay snapshots exact bytes before concurrent request cleanup", async () => {
  const input = acceptedInput(new TextEncoder().encode('{"prompt":"retain exact bytes"}'));
  const expectedBody = new Uint8Array(input.body);
  class CleanupRaceKv extends CountingKv {
    override get<T = unknown>(
      key: Deno.KvKey,
      options?: Readonly<{ consistency?: "strong" | "eventual" }>,
    ): Promise<Deno.KvEntryMaybe<T>> {
      input.body.fill(0);
      return super.get<T>(key, options);
    }
  }
  const kv = new CleanupRaceKv();
  const persisted = await persistEncryptedSentinelReplay(input, failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => 1_777_000_000_000,
    randomUuid: () => "capture-cleanup-race",
    randomBytes: () => new Uint8Array(12).fill(8),
  });
  assert.equal(persisted.status, "stored");
  assert.deepEqual(new Set(input.body), new Set([0]));
  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  });
  assert.equal(page.captures.length, 1);
  const decrypted = await decryptExportedSentinelReplay(page.captures[0]!, keyBytes);
  assert.deepEqual(decrypted.body, expectedBody);
  decrypted.body.fill(0);
  expectedBody.fill(0);
});

Deno.test("sentinel manifest export seeks to the requested timestamp and returns one ordered item", async () => {
  const backing = new CountingKv();
  await persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
    kv: backing as unknown as Deno.Kv,
    keyBytes,
    now: () => 1_777_000_000_000,
    randomUuid: () => "ordered-fixture",
  });
  const [fixture] = (await listEncryptedSentinelReplays(backing as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  })).captures;
  assert.ok(fixture);
  let observedSelector: Deno.KvListSelector | null = null;
  let observedOptions: Deno.KvListOptions | undefined;
  const kv = {
    list(selector: Deno.KvListSelector, options?: Deno.KvListOptions) {
      observedSelector = selector;
      observedOptions = options;
      const iterator = (async function* () {
        yield {
          key: [
            ...SENTINEL_REPLAY_MANIFEST_PREFIX,
            fixture.manifest.captured_at_ms,
            fixture.manifest.fingerprint,
            fixture.manifest.capture_id,
          ],
          value: fixture.manifest,
          versionstamp: "0000000000000001",
        };
      })() as unknown as Deno.KvListIterator<typeof fixture.manifest>;
      Object.defineProperty(iterator, "cursor", { get: () => "next_cursor==" });
      return iterator;
    },
    getMany(keys: readonly Deno.KvKey[]) {
      return Promise.resolve(keys.map((key) => {
        const index = Number(key.at(-1));
        return {
          key,
          value: base64UrlDecode(fixture.chunks[index]!),
          versionstamp: "0000000000000001",
        };
      }));
    },
  };
  const afterMs = fixture.manifest.captured_at_ms;
  const beforeMs = afterMs + 10;
  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs,
    beforeMs,
    limit: 1,
    cursor: "prior_cursor==",
  });
  assert.equal(page.captures.length, 1);
  assert.equal(page.cursor, "next_cursor==");
  const selector = observedSelector as unknown as Deno.KvListSelector;
  assert.ok("prefix" in selector);
  if (!("prefix" in selector)) throw new Error("fixture selector missing prefix");
  assert.deepEqual(selector.prefix, SENTINEL_REPLAY_MANIFEST_PREFIX);
  assert.ok("start" in selector);
  if (!("start" in selector)) throw new Error("fixture selector missing start");
  assert.deepEqual(selector.start, [...SENTINEL_REPLAY_MANIFEST_PREFIX, afterMs]);
  assert.equal("end" in selector, false);
  assert.equal(observedOptions?.cursor, "prior_cursor==");
  assert.equal(observedOptions?.limit, 1);
});

Deno.test("terminal logging persists buffered failures but never successful responses", async () => {
  const input = acceptedInput();
  const observations: SentinelFailureObservation[] = [];
  const clientObservations: SentinelClientFailureObservation[] = [];
  const persist = (
    _capture: AcceptedSentinelReplayInput,
    observation: SentinelFailureObservation,
    clientObservation?: SentinelClientFailureObservation,
  ): Promise<{ status: "duplicate"; fingerprint: string }> => {
    observations.push(observation);
    if (clientObservation) clientObservations.push(clientObservation);
    return Promise.resolve({ status: "duplicate", fingerprint: "fixture" });
  };
  const recordTelemetry = () =>
    Promise.resolve({
      status: "ignored" as const,
      reason: "unknown_release" as const,
      release: null,
      provider: null,
      route: null,
      model_hash: null,
    });

  const failure = await withTerminalRequestLog(
    new Response('{"error":{}}', { status: 502, headers: { "Content-Type": "application/json" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "failure",
      sentinelReplayInput: input,
      persistSentinelReplay: persist,
      recordTelemetry,
    },
  );
  assert.equal(failure.status, 502);
  assert.equal(observations.length, 1);

  const semanticFailure = await withTerminalRequestLog(
    new Response('{"object":"embeddings.job","status":"failed","error":{"code":"provider_error"}}', {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    {
      route: "embeddings.jobs.create",
      startedAtMonotonicMs: performance.now(),
      requestId: "semantic-failure",
      sentinelReplayInput: input,
      persistSentinelReplay: persist,
      recordTelemetry,
    },
  );
  assert.equal(semanticFailure.status, 200);
  assert.equal(observations.length, 2);
  assert.deepEqual(clientObservations[1], {
    status: 200,
    stream: false,
    completed: false,
    terminal_type: "job.failed",
    failure_kind: "provider_error",
    framing_valid: true,
    provider_route: "gateway",
  });

  const success = await withTerminalRequestLog(
    new Response('{"id":"response"}', { status: 200, headers: { "Content-Type": "application/json" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "success",
      sentinelReplayInput: input,
      persistSentinelReplay: persist,
      recordTelemetry,
    },
  );
  assert.equal(success.status, 200);
  assert.equal(observations.length, 2);
});

Deno.test("buffered replay persistence precedes delivery completion and clears request bytes", async () => {
  const capture = acceptedInput();
  const originalBytes = [...capture.body];
  const delivery = Promise.withResolvers<void>();
  let persisted = false;
  const response = await withTerminalRequestLog(
    new Response('{"error":{"code":"provider_transport"}}', {
      status: 502,
      headers: { "Content-Type": "application/json" },
    }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "delivery-stalled",
      deliveryCompleted: delivery.promise,
      deliverySignal: new AbortController().signal,
      sentinelReplayInput: capture,
      persistSentinelReplay: () => {
        persisted = true;
        assert.deepEqual([...capture.body], originalBytes);
        return Promise.resolve({ status: "duplicate", fingerprint: "fixture" });
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
    },
  );
  assert.equal(response.status, 502);
  assert.equal(persisted, true);
  assert.ok(capture.body.every((byte) => byte === 0));
  delivery.resolve();
  await new Promise((resolve) => setTimeout(resolve, 0));

  const successfulCapture = acceptedInput();
  await withTerminalRequestLog(
    new Response('{"id":"response"}', { status: 200, headers: { "Content-Type": "application/json" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "successful-clear",
      sentinelReplayInput: successfulCapture,
      persistSentinelReplay: () => Promise.reject(new Error("successful response must not persist")),
      recordTelemetry: () =>
        Promise.resolve({
          status: "ignored" as const,
          reason: "unknown_release" as const,
          release: null,
          provider: null,
          route: null,
          model_hash: null,
        }),
    },
  );
  assert.ok(successfulCapture.body.every((byte) => byte === 0));
});

Deno.test("client observation gives SSE failures precedence and recognizes buffered semantic failures", () => {
  const inspector = createSentinelSseInspector();
  inspector.push(new TextEncoder().encode(
    `data: {"type":"response.completed","response":{"status":"completed"}}\n\n` +
      `data: {"type":"response.failed","response":{"status":"failed","error":{"code":"server_error"}}}\n\n`,
  ));
  assert.deepEqual(inspector.finish(), {
    stream: true,
    completed: false,
    terminal_type: "response.failed",
    failure_kind: "server_error",
    framing_valid: true,
  });

  const missingTerminal = createSentinelSseInspector();
  missingTerminal.push(new TextEncoder().encode('data: {"type":"response.output_text.delta","delta":"x"}\n\n'));
  assert.deepEqual(missingTerminal.finish(), {
    stream: true,
    completed: false,
    terminal_type: null,
    failure_kind: "missing_sse_terminal",
    framing_valid: false,
  });
  const readFailure = createSentinelSseInspector();
  readFailure.push(new TextEncoder().encode('data: {"type":"response.output_text.delta","delta":"x"}\n\n'));
  assert.deepEqual(readFailure.finish("read_error"), {
    stream: true,
    completed: false,
    terminal_type: null,
    failure_kind: "stream_read_error",
    framing_valid: false,
  });
  assert.deepEqual(
    resolveSentinelClientFailureObservation(
      failedObservation({
        status: 200,
        stream: true,
        terminal_type: "response.incomplete",
        failure_kind: "provider_internal_deadline",
      }),
      {
        stream: true,
        completed: false,
        terminal_type: "response.incomplete",
        failure_kind: null,
        framing_valid: true,
      },
    ),
    {
      status: 200,
      stream: true,
      completed: false,
      terminal_type: "response.incomplete",
      failure_kind: null,
      framing_valid: true,
      provider_route: "chatgpt_codex",
    },
  );

  assert.deepEqual(
    inspectSentinelBufferedResponse(
      200,
      "application/json",
      new TextEncoder().encode('{"object":"embeddings.job","status":"failed","error":{"code":"provider_error"}}'),
    ),
    {
      stream: false,
      completed: false,
      terminal_type: "job.failed",
      failure_kind: "provider_error",
      framing_valid: true,
    },
  );
  assert.deepEqual(
    inspectSentinelBufferedResponse(
      202,
      "application/json",
      new TextEncoder().encode('{"object":"embeddings.job","status":"queued","retry_after_seconds":1}'),
    ),
    {
      stream: false,
      completed: false,
      terminal_type: "job.queued",
      failure_kind: null,
      framing_valid: true,
    },
  );
});

Deno.test("buffered response inspection stops at its declared and streamed byte bounds", async () => {
  const declared = new Response("{}", {
    status: 502,
    headers: {
      "Content-Type": "application/json",
      "Content-Length": String(SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES + 1),
    },
  });
  assert.equal(await inspectSentinelBufferedResponseBody(declared), null);
  assert.equal(await declared.text(), "{}");

  const streamed = new Response(
    new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(new Uint8Array(SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES + 1));
        controller.close();
      },
    }),
    { status: 502, headers: { "Content-Type": "application/json" } },
  );
  assert.equal(await inspectSentinelBufferedResponseBody(streamed), null);
  await streamed.body?.cancel();
});

Deno.test("client-visible signatures survive encrypted capture round trips", async () => {
  const kv = new CountingKv();
  const internal = failedObservation({
    status: 200,
    stream: true,
    terminal_type: "eof",
    failure_kind: "premature_eof",
    synthetic_terminal_type: "response.failed",
  });
  const client = failedClientObservation({
    status: 200,
    stream: true,
    terminal_type: "response.failed",
    failure_kind: "server_error",
  });
  const stored = await persistEncryptedSentinelReplay(
    acceptedInput(),
    internal,
    { kv: kv as unknown as Deno.Kv, keyBytes, now: () => 1_777_000_000_000 },
    client,
  );
  assert.equal(stored.status, "stored");
  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  });
  const decrypted = await decryptExportedSentinelReplay(page.captures[0]!, keyBytes);
  assert.deepEqual(decrypted.observation, internal);
  assert.deepEqual(decrypted.client_observation, client);
  assert.equal(decrypted.failure_signature, sentinelFailureSignature(client));
});

Deno.test("encrypted replay rejects hostile manifest and chunk sizes before decryption", async () => {
  const kv = new CountingKv();
  await persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => 1_777_000_000_000,
  });
  const page = await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  });
  const capture = page.captures[0]!;
  assert.equal(
    isExportedSentinelReplayCapture({
      ...capture,
      manifest: { ...capture.manifest, iv: base64UrlEncode(new Uint8Array(11)) },
    }),
    false,
  );
  assert.equal(
    isExportedSentinelReplayCapture({
      ...capture,
      chunks: ["A".repeat(Math.ceil(SENTINEL_REPLAY_CHUNK_BYTES / 3) * 4 + 1)],
    }),
    false,
  );
  assert.equal(
    isExportedSentinelReplayCapture({
      ...capture,
      manifest: { ...capture.manifest, ciphertext_bytes: Number.MAX_SAFE_INTEGER },
    }),
    false,
  );
});

Deno.test("stream capture distinguishes gateway read failure from downstream cancellation", async () => {
  const input = acceptedInput();
  const observations: SentinelFailureObservation[] = [];
  const persist = (
    _capture: AcceptedSentinelReplayInput,
    observation: SentinelFailureObservation,
  ): Promise<{ status: "duplicate"; fingerprint: string }> => {
    observations.push(observation);
    return Promise.resolve({ status: "duplicate", fingerprint: "fixture" });
  };
  const recordTelemetry = () =>
    Promise.resolve({
      status: "ignored" as const,
      reason: "unknown_release" as const,
      release: null,
      provider: null,
      route: null,
      model_hash: null,
    });

  const failedBody = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(new TextEncoder().encode("data: {}\n\n"));
      controller.error(new Error("gateway body failed"));
    },
  });
  const failedResponse = await withTerminalRequestLog(
    new Response(failedBody, { headers: { "Content-Type": "text/event-stream" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "stream-failed",
      sentinelReplayInput: input,
      persistSentinelReplay: persist,
      recordTelemetry,
    },
  );
  await assert.rejects(() => failedResponse.text());
  assert.equal(observations.length, 1);
  assert.equal(observations[0]!.failure_kind, "gateway_stream_read_error");

  const downstream = new AbortController();
  const delivery = Promise.withResolvers<void>();
  const cancelledBody = new ReadableStream<Uint8Array>({
    async pull() {
      await new Promise((_resolve, reject) =>
        downstream.signal.addEventListener(
          "abort",
          () => reject(new DOMException("client disconnected", "AbortError")),
          { once: true },
        )
      );
    },
  });
  const cancelledResponse = await withTerminalRequestLog(
    new Response(cancelledBody, { status: 502, headers: { "Content-Type": "text/event-stream" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "stream-cancelled",
      deliveryCompleted: delivery.promise,
      deliverySignal: downstream.signal,
      sentinelReplayInput: input,
      persistSentinelReplay: persist,
      recordTelemetry,
    },
  );
  const consumption = cancelledResponse.text().catch(() => "");
  await new Promise((resolve) => setTimeout(resolve, 0));
  downstream.abort();
  delivery.reject(new Error("client disconnected"));
  await consumption;
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(observations.length, 1);
});

Deno.test("stream cancellation while a provider read resolves done never persists replay", async () => {
  const capture = acceptedInput();
  const providerPullStarted = Promise.withResolvers<void>();
  const providerCancelled = Promise.withResolvers<void>();
  let pullStarted = false;
  let persisted = 0;
  const pendingBody = new ReadableStream<Uint8Array>({
    pull() {
      if (!pullStarted) {
        pullStarted = true;
        providerPullStarted.resolve();
      }
      return new Promise<void>(() => {});
    },
    cancel() {
      providerCancelled.resolve();
    },
  });
  const response = await withTerminalRequestLog(
    new Response(pendingBody, { headers: { "Content-Type": "text/event-stream" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "stream-cancelled-pending-read",
      sentinelReplayInput: capture,
      persistSentinelReplay: () => {
        persisted += 1;
        return Promise.resolve({ status: "duplicate", fingerprint: "fixture" });
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
    },
  );
  const reader = response.body!.getReader();
  const read = reader.read();
  await providerPullStarted.promise;
  await reader.cancel("client disconnected");
  await providerCancelled.promise;
  await read;
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(persisted, 0);
  assert.ok(capture.body.every((byte) => byte === 0));
});

Deno.test("SSE provider read failure persists and clears request bytes before delivery settles", async () => {
  const capture = acceptedInput();
  const originalBytes = [...capture.body];
  const delivery = Promise.withResolvers<void>();
  let persisted = false;
  const failedBody = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(new TextEncoder().encode('data: {"type":"response.output_text.delta","delta":"x"}\n\n'));
      controller.error(new Error("provider stream read failed"));
    },
  });
  const response = await withTerminalRequestLog(
    new Response(failedBody, { headers: { "Content-Type": "text/event-stream" } }),
    {
      route: "responses",
      startedAtMonotonicMs: performance.now(),
      requestId: "stream-failed-delivery-stalled",
      deliveryCompleted: delivery.promise,
      deliverySignal: new AbortController().signal,
      sentinelReplayInput: capture,
      persistSentinelReplay: (_input, observation, clientObservation) => {
        persisted = true;
        assert.deepEqual([...capture.body], originalBytes);
        assert.equal(observation.failure_kind, "gateway_stream_read_error");
        assert.equal(clientObservation?.failure_kind, "stream_read_error");
        assert.equal(clientObservation?.framing_valid, false);
        return Promise.resolve({ status: "duplicate", fingerprint: "fixture" });
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
    },
  );
  await assert.rejects(() => response.text(), /provider stream read failed/);
  assert.equal(persisted, true);
  assert.ok(capture.body.every((byte) => byte === 0));
  delivery.resolve();
  await new Promise((resolve) => setTimeout(resolve, 0));
});

Deno.test("sentinel replay admin separates invalid input from storage failure", async () => {
  let getKvCalled = false;
  const invalid = await handleAdminSentinelReplayCaptures(
    new Request("https://ai.ubq.fi/admin/sentinel/replay-captures?cursor=not%20opaque"),
    {
      getKv: () => {
        getKvCalled = true;
        return Promise.resolve(null);
      },
    },
  );
  assert.equal(invalid.status, 400);
  assert.equal(getKvCalled, false);

  const excessiveLimit = await handleAdminSentinelReplayCaptures(
    new Request("https://ai.ubq.fi/admin/sentinel/replay-captures?limit=2"),
    {
      getKv: () => {
        getKvCalled = true;
        return Promise.resolve(null);
      },
    },
  );
  assert.equal(excessiveLimit.status, 400);
  assert.equal(getKvCalled, false);
  assert.equal(SENTINEL_REPLAY_EXPORT_PAGE_LIMIT, 1);

  const unavailable = await handleAdminSentinelReplayCaptures(
    new Request("https://ai.ubq.fi/admin/sentinel/replay-captures?before_ms=2000000000000&cursor=valid_cursor%3D%3D"),
    {
      getKv: () => Promise.resolve({} as Deno.Kv),
      listEncryptedSentinelReplays: () => Promise.reject(new Error("KV unavailable")),
    },
  );
  assert.equal(unavailable.status, 503);

  const kvFailure = await handleAdminSentinelReplayCaptures(
    new Request("https://ai.ubq.fi/admin/sentinel/replay-captures?before_ms=2000000000000"),
    { getKv: () => Promise.reject(new Error("KV open failed")) },
  );
  assert.equal(kvFailure.status, 503);
});

Deno.test("sentinel replay admin forwards the closed capture interval", async () => {
  let observed: { afterMs: number; beforeMs: number; cursor?: string } | null = null;
  const response = await handleAdminSentinelReplayCaptures(
    new Request(
      "https://ai.ubq.fi/admin/sentinel/replay-captures?after_ms=100&before_ms=200&limit=1&cursor=opaque%3D",
    ),
    {
      getKv: () => Promise.resolve({} as Deno.Kv),
      listEncryptedSentinelReplays: (_kv, options) => {
        observed = { afterMs: options.afterMs, beforeMs: options.beforeMs, cursor: options.cursor };
        return Promise.resolve({ captures: [], cursor: "" });
      },
    },
  );
  assert.equal(response.status, 200);
  assert.deepEqual(observed, { afterMs: 100, beforeMs: 200, cursor: "opaque=" });

  const incidentId = "provider-00000000-0000-4000-8000-000000000001";
  let observedIncident: { incidentId: string; cursor?: string } | null = null;
  const incidentResponse = await handleAdminSentinelReplayCaptures(
    new Request(
      `https://ai.ubq.fi/admin/sentinel/replay-captures?after_ms=100&before_ms=200&limit=1&incident_id=${incidentId}`,
    ),
    {
      getKv: () => Promise.resolve({} as Deno.Kv),
      listEncryptedSentinelIncidentReplays: (_kv, options) => {
        observedIncident = { incidentId: options.incidentId, cursor: options.cursor };
        return Promise.resolve({ captures: [], cursor: "" });
      },
    },
  );
  assert.equal(incidentResponse.status, 200);
  assert.deepEqual(observedIncident, { incidentId, cursor: undefined });
});

Deno.test("replay export and preview reads enforce declared and streamed byte limits", async () => {
  let requestedLimit: string | null = null;
  await assert.rejects(
    () =>
      fetchEncryptedReplayCaptures({
        baseUrl: "https://preview.example",
        adminToken: "admin-fixture",
        afterMs: 0,
        beforeMs: 2_000_000_000_000,
        fetchImpl: ((input: URL | Request | string) => {
          const url = input instanceof Request ? new URL(input.url) : new URL(String(input));
          requestedLimit = url.searchParams.get("limit");
          return Promise.resolve(
            new Response(null, {
              status: 200,
              headers: { "Content-Length": String(SENTINEL_MAX_ENCRYPTED_REPLAY_PAGE_BYTES + 1) },
            }),
          );
        }) as typeof fetch,
      }),
    /size limit/,
  );
  assert.equal(requestedLimit, "1");

  const original = failedClientObservation({ status: 200, stream: true });
  const replayCase: ReplayCase = {
    fingerprint: "f".repeat(64),
    case_group_digest: "c".repeat(64),
    captured_at_ms: 1_777_000_000_000,
    endpoint: "/v1/responses",
    method: "POST",
    content_type: "application/json",
    compatibility_headers: { accept: "text/event-stream" },
    body: new TextEncoder().encode('{"model":"gpt-5.6-sol"}'),
    original: { ...original, failure_signature: sentinelFailureSignature(original) },
  };

  let declaredBodyCancelled = false;
  const declaredResult = await replayOneCase({
    replayCase,
    previewBaseUrl: "https://preview.example",
    previewCredential: "preview-fixture",
    fetchImpl: (() =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            cancel() {
              declaredBodyCancelled = true;
            },
          }),
          {
            status: 200,
            headers: {
              "Content-Type": "text/event-stream",
              "Content-Length": String(SENTINEL_MAX_REPLAY_RESPONSE_BYTES + 1),
            },
          },
        ),
      )) as typeof fetch,
  });
  assert.equal(declaredResult.outcome, "unavailable");
  assert.equal(declaredResult.unavailable_reason, "response_too_large");
  assert.equal(declaredBodyCancelled, true);

  const chunk = new Uint8Array(1_024 * 1_024).fill(32);
  let pulls = 0;
  let streamedBodyCancelled = false;
  const streamedResult = await replayOneCase({
    replayCase,
    previewBaseUrl: "https://preview.example",
    previewCredential: "preview-fixture",
    fetchImpl: (() =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            pull(controller) {
              pulls += 1;
              controller.enqueue(chunk);
            },
            cancel() {
              streamedBodyCancelled = true;
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        ),
      )) as typeof fetch,
  });
  assert.equal(streamedResult.outcome, "unavailable");
  assert.equal(streamedResult.unavailable_reason, "response_too_large");
  const pullsNeededToExceedLimit = SENTINEL_MAX_REPLAY_RESPONSE_BYTES / chunk.byteLength + 1;
  assert.ok(pulls >= pullsNeededToExceedLimit && pulls <= pullsNeededToExceedLimit + 1);
  assert.equal(streamedBodyCancelled, true);

  const readErrorOriginal = failedClientObservation({
    status: 200,
    stream: true,
    completed: false,
    terminal_type: null,
    failure_kind: "stream_read_error",
    framing_valid: false,
    provider_route: "gateway",
  });
  let eventSent = false;
  const readErrorResult = await replayOneCase({
    replayCase: {
      ...replayCase,
      original: {
        ...readErrorOriginal,
        failure_signature: sentinelFailureSignature(readErrorOriginal),
      },
    },
    previewBaseUrl: "https://preview.example",
    previewCredential: "preview-fixture",
    fetchImpl: (() =>
      Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            pull(controller) {
              if (!eventSent) {
                eventSent = true;
                controller.enqueue(
                  new TextEncoder().encode('data: {"type":"response.output_text.delta","delta":"x"}\n\n'),
                );
                return;
              }
              controller.error(new Error("preview stream failed"));
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        ),
      )) as typeof fetch,
  });
  assert.equal(readErrorResult.attempted, true);
  assert.equal(readErrorResult.sse_framing_valid, false);
  assert.equal(readErrorResult.outcome, "same_failure");
  assert.equal(readErrorResult.comparison.failure_signature_matches_original, true);
  assert.equal(readErrorResult.comparison.framing_matches_original, true);
});

Deno.test("replay export pagination rejects repeated cursors and stops at the fixed page bound", async () => {
  const kv = new CountingKv();
  await persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => 1_777_000_000_000,
    randomUuid: () => "pagination-fixture",
  });
  const [fixture] = (await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  })).captures;
  assert.ok(fixture);
  const pageCapture = (index: number) => ({
    ...fixture,
    manifest: {
      ...fixture.manifest,
      capture_id: `pagination-${index}`,
      fingerprint: index.toString(16).padStart(64, "0"),
      captured_at_ms: fixture.manifest.captured_at_ms + index,
      expires_at_ms: fixture.manifest.captured_at_ms + index + SENTINEL_REPLAY_TTL_MS,
    },
  });

  let repeatedCalls = 0;
  await assert.rejects(
    () =>
      fetchEncryptedReplayCaptures({
        baseUrl: "https://preview.example",
        adminToken: "admin-fixture",
        afterMs: 0,
        beforeMs: 2_000_000_000_000,
        fetchImpl: ((input: URL | Request | string) => {
          const index = repeatedCalls++;
          const url = input instanceof Request ? new URL(input.url) : new URL(String(input));
          assert.equal(url.searchParams.get("cursor"), index === 0 ? null : "repeated_cursor==");
          return Promise.resolve(
            Response.json({ data: [pageCapture(index)], cursor: "repeated_cursor==" }),
          );
        }) as typeof fetch,
      }),
    /cursor repeated/,
  );
  assert.equal(repeatedCalls, 2);

  let boundedCalls = 0;
  await assert.rejects(
    () =>
      fetchEncryptedReplayCaptures({
        baseUrl: "https://preview.example",
        adminToken: "admin-fixture",
        afterMs: 0,
        beforeMs: 2_000_000_000_000,
        fetchImpl: (() => {
          const index = boundedCalls++;
          return Promise.resolve(
            Response.json({ data: [pageCapture(index)], cursor: `cursor_${index}` }),
          );
        }) as typeof fetch,
      }),
    /page limit/,
  );
  assert.equal(boundedCalls, SENTINEL_MAX_REPLAY_EXPORT_PAGES);
});

Deno.test("replay export accepts only manifests inside the fixed log interval", async () => {
  const kv = new CountingKv();
  await persistEncryptedSentinelReplay(acceptedInput(), failedObservation(), {
    kv: kv as unknown as Deno.Kv,
    keyBytes,
    now: () => 1_777_000_000_000,
    randomUuid: () => "range-fixture",
  });
  const [fixture] = (await listEncryptedSentinelReplays(kv as unknown as Deno.Kv, {
    afterMs: 0,
    beforeMs: Number.MAX_SAFE_INTEGER - 1,
  })).captures;
  assert.ok(fixture);
  const afterMs = fixture.manifest.captured_at_ms - 1;
  const beforeMs = fixture.manifest.captured_at_ms + 1;
  let observedBefore: string | null = null;
  const inRange = await fetchEncryptedReplayCaptures({
    baseUrl: "https://preview.example",
    adminToken: "admin-fixture",
    afterMs,
    beforeMs,
    fetchImpl: ((input: URL | Request | string) => {
      const url = input instanceof Request ? new URL(input.url) : new URL(String(input));
      observedBefore = url.searchParams.get("before_ms");
      return Promise.resolve(Response.json({ data: [fixture], cursor: null }));
    }) as typeof fetch,
  });
  assert.equal(observedBefore, String(beforeMs));
  assert.equal(inRange.length, 1);

  for (const capturedAtMs of [afterMs - 1, beforeMs + 1]) {
    const outside = {
      ...fixture,
      manifest: {
        ...fixture.manifest,
        capture_id: `outside-${capturedAtMs}`,
        captured_at_ms: capturedAtMs,
        expires_at_ms: capturedAtMs + SENTINEL_REPLAY_TTL_MS,
      },
    };
    await assert.rejects(
      () =>
        fetchEncryptedReplayCaptures({
          baseUrl: "https://preview.example",
          adminToken: "admin-fixture",
          afterMs,
          beforeMs,
          fetchImpl: (() => Promise.resolve(Response.json({ data: [outside], cursor: null }))) as typeof fetch,
        }),
      /out-of-range manifest/,
    );
  }
});
