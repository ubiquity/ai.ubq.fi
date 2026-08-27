import assert from "node:assert/strict";
import {
  acknowledgeSentinelIncident,
  claimSentinelIncidentWorkflowRun,
  coalesceSentinelIncidentFailureEvents,
  completeSentinelIncidentFailureEvent,
  createSentinelGitHubAppJwt,
  createSentinelIncidentFailureEvent,
  deferSentinelIncident,
  isSentinelIncidentControl,
  isSentinelProductionRuntime,
  MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS,
  reconcileSentinelIncidentOutbox,
  reconcileSentinelIncidentOutboxFromEnvironment,
  recordSentinelIncidentFailure,
  recordSentinelProviderDegradation,
  SENTINEL_INCIDENT_CAPTURE_REF_PREFIX,
  SENTINEL_INCIDENT_CONTROL_KEY,
  SENTINEL_INCIDENT_DEAD_PREFIX,
  SentinelIncidentAckConflict,
  SentinelIncidentClaimConflict,
  type SentinelIncidentControl,
  SentinelIncidentDeferConflict,
} from "../src/sentinel_incident_outbox.ts";
import { base64UrlDecode } from "../src/utils.ts";
import {
  handleAdminSentinelIncidentAck,
  handleAdminSentinelIncidentClaim,
  handleAdminSentinelIncidentDefer,
} from "../src/sentinel_incident_admin.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const concatBytes = (...parts: readonly Uint8Array[]): Uint8Array => {
  const combined = new Uint8Array(parts.reduce((length, part) => length + part.byteLength, 0));
  let offset = 0;
  for (const part of parts) {
    combined.set(part, offset);
    offset += part.byteLength;
  }
  return combined;
};

const derLength = (length: number): Uint8Array => {
  if (length < 0x80) return Uint8Array.of(length);
  const octets: number[] = [];
  for (let remaining = length; remaining > 0; remaining >>>= 8) octets.unshift(remaining & 0xff);
  return Uint8Array.of(0x80 | octets.length, ...octets);
};

const derValue = (tag: number, value: Uint8Array): Uint8Array =>
  concatBytes(Uint8Array.of(tag), derLength(value.byteLength), value);

const base64ToBytes = (value: string): Uint8Array => {
  const decoded = atob(value.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - (value.length % 4)) % 4));
  return Uint8Array.from(decoded, (character) => character.charCodeAt(0));
};

const bytesToBase64 = (value: Uint8Array): string => {
  let binary = "";
  for (const byte of value) binary += String.fromCharCode(byte);
  return btoa(binary);
};

const pem = (label: string, value: Uint8Array): string => {
  const lines = bytesToBase64(value).match(/.{1,64}/g) ?? [];
  return `-----BEGIN ${label}-----\n${lines.join("\n")}\n-----END ${label}-----\n`;
};

const derInteger = (value: Uint8Array): Uint8Array => {
  let start = 0;
  while (start < value.byteLength - 1 && value[start] === 0) start += 1;
  const unsigned = value.slice(start);
  return derValue(0x02, unsigned[0]! & 0x80 ? concatBytes(Uint8Array.of(0), unsigned) : unsigned);
};

const requireJwkPart = (jwk: JsonWebKey, key: "n" | "e" | "d" | "p" | "q" | "dp" | "dq" | "qi"): Uint8Array => {
  const value = jwk[key];
  if (typeof value !== "string") throw new Error(`missing RSA JWK ${key}`);
  return base64ToBytes(value);
};

const pkcs1FromPrivateKey = async (privateKey: CryptoKey): Promise<Uint8Array> => {
  const jwk = await crypto.subtle.exportKey("jwk", privateKey);
  return derValue(
    0x30,
    concatBytes(
      derInteger(Uint8Array.of(0)),
      derInteger(requireJwkPart(jwk, "n")),
      derInteger(requireJwkPart(jwk, "e")),
      derInteger(requireJwkPart(jwk, "d")),
      derInteger(requireJwkPart(jwk, "p")),
      derInteger(requireJwkPart(jwk, "q")),
      derInteger(requireJwkPart(jwk, "dp")),
      derInteger(requireJwkPart(jwk, "dq")),
      derInteger(requireJwkPart(jwk, "qi")),
    ),
  );
};

const keyPair = await crypto.subtle.generateKey(
  {
    name: "RSASSA-PKCS1-v1_5",
    modulusLength: 2048,
    publicExponent: Uint8Array.of(1, 0, 1),
    hash: "SHA-256",
  },
  true,
  ["sign", "verify"],
);
const pkcs8Pem = pem("PRIVATE KEY", new Uint8Array(await crypto.subtle.exportKey("pkcs8", keyPair.privateKey)));
const pkcs1Pem = pem("RSA PRIVATE KEY", await pkcs1FromPrivateKey(keyPair.privateKey));
const NOW = 1_777_000_000_000;
const UUID_ONE = "00000000-0000-4000-8000-000000000001";
const UUID_TWO = "00000000-0000-4000-8000-000000000002";
const ACK_ONE = "A".repeat(43);
const ACK_TWO = "B".repeat(43);

const decodeJsonPart = (value: string): Record<string, unknown> =>
  JSON.parse(new TextDecoder().decode(base64UrlDecode(value))) as Record<string, unknown>;

const githubFetcher = (
  requests: Request[],
  options: Readonly<{
    dispatchStatus?: number;
    runConclusion?: string | null;
    runId?: number;
    tokenExpiresAt?: number;
  }> = {},
): typeof fetch =>
async (input, init) => {
  const request = new Request(input, init);
  requests.push(request.clone());
  await request.clone().arrayBuffer();
  const url = new URL(request.url);
  const runId = options.runId ?? 12345;
  if (url.pathname.endsWith("/access_tokens")) {
    return Response.json(
      {
        token: `ghs_${"x".repeat(600)}`,
        expires_at: new Date(options.tokenExpiresAt ?? NOW + 3_600_000).toISOString(),
      },
      {
        status: 201,
      },
    );
  }
  if (url.pathname.endsWith("/dispatches")) {
    const status = options.dispatchStatus ?? 200;
    return status === 200
      ? Response.json({
        workflow_run_id: runId,
        run_url: `https://api.github.com/repos/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
        html_url: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
      })
      : new Response("private-response-marker", { status });
  }
  if (url.pathname.endsWith(`/actions/runs/${runId}`)) {
    return Response.json({
      id: runId,
      status: "completed",
      conclusion: options.runConclusion ?? "failure",
      html_url: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
      head_sha: "a".repeat(40),
    });
  }
  throw new Error("unexpected GitHub request");
};

const readControl = async (kv: CountingKv): Promise<SentinelIncidentControl> => {
  const value = (await kv.get<SentinelIncidentControl>(SENTINEL_INCIDENT_CONTROL_KEY)).value;
  assert.ok(isSentinelIncidentControl(value));
  return value;
};

Deno.test("pre-deferral v1 incident batches remain readable as zero infrastructure deferrals", async () => {
  const kv = new CountingKv();
  await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  const control = await readControl(kv);
  const legacyActive = { ...control.active } as Record<string, unknown>;
  delete legacyActive.infrastructure_deferrals;
  assert.equal(isSentinelIncidentControl({ ...control, active: legacyActive }), true);
});

Deno.test("sentinel GitHub App JWT supports GitHub PKCS#1 keys and has verifiable fixed claims", async () => {
  for (const value of [pkcs8Pem, pkcs1Pem, pkcs1Pem.replaceAll("\n", "\\n")]) {
    const jwt = await createSentinelGitHubAppJwt(value, NOW);
    const parts = jwt.split(".");
    assert.equal(parts.length, 3);
    assert.deepEqual(decodeJsonPart(parts[0]!), { alg: "RS256", typ: "JWT" });
    assert.deepEqual(decodeJsonPart(parts[1]!), {
      iat: Math.floor(NOW / 1_000) - 60,
      exp: Math.floor(NOW / 1_000) + 540,
      iss: "4682172",
    });
    assert.equal(
      await crypto.subtle.verify(
        "RSASSA-PKCS1-v1_5",
        keyPair.publicKey,
        base64UrlDecode(parts[2]!),
        new TextEncoder().encode(`${parts[0]}.${parts[1]}`),
      ),
      true,
    );
  }
});

Deno.test("sentinel incident dispatch uses the exact least-privilege App and workflow contract", async () => {
  const kv = new CountingKv();
  const first = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  const second = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW + 1, {
    randomUuid: () => UUID_TWO,
  });
  assert.equal(first.incidentId, `provider-${UUID_ONE}`);
  assert.equal(second.incidentId, first.incidentId);
  assert.equal(second.created, false);
  const requests: Request[] = [];
  const result = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs1Pem, {
    now: () => NOW + 2,
    fetcher: githubFetcher(requests),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.deepEqual(result, {
    status: "dispatched",
    incidentId: first.incidentId,
    attempt: 1,
    workflowRunId: 12345,
  });
  assert.equal(requests.length, 2);
  assert.equal(new URL(requests[0]!.url).pathname, "/app/installations/155687488/access_tokens");
  assert.deepEqual(await requests[0]!.json(), { repositories: ["ai.ubq.fi"], permissions: { actions: "write" } });
  assert.equal(
    new URL(requests[1]!.url).pathname,
    "/repos/ubiquity/ai.ubq.fi/actions/workflows/provider-sentinel.yml/dispatches",
  );
  assert.deepEqual(await requests[1]!.json(), {
    ref: "development",
    inputs: {
      sentinel_mode: "incident",
      incident_id: first.incidentId,
      incident_attempt: "1",
      incident_start_ms: String(NOW),
      incident_ack_nonce: ACK_ONE,
    },
    return_run_details: true,
  });
  const control = await readControl(kv);
  assert.equal(control.active?.state, "dispatched");
  assert.equal(control.active?.failure_count, 2);
  assert.equal(control.active?.workflow_run_id, 12345);
  const serialized = JSON.stringify(control);
  for (const forbidden of ["request-one", "/v1/responses", "Authorization", "private-response-marker"]) {
    assert.equal(serialized.includes(forbidden), false);
  }
});

Deno.test("sentinel incidents coalesce behind one active repair and ACK promotes one successor", async () => {
  const kv = new CountingKv();
  const first = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  const pending = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW + 10, {
    randomUuid: () => UUID_TWO,
    randomAckNonce: () => ACK_TWO,
  });
  await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW + 11, { randomUuid: () => UUID_ONE });
  assert.notEqual(pending.incidentId, first.incidentId);
  let control = await readControl(kv);
  assert.equal(control.active?.id, first.incidentId);
  assert.equal(control.pending?.id, pending.incidentId);
  assert.equal(control.pending?.failure_count, 2);
  assert.equal(
    await acknowledgeSentinelIncident(
      kv as unknown as Deno.Kv,
      { incidentId: first.incidentId, attempt: 1, workflowRunId: 12345, ackNonce: ACK_ONE },
      NOW + 20,
    ),
    "acknowledged",
  );
  control = await readControl(kv);
  assert.equal(control.active?.id, pending.incidentId);
  assert.equal(control.active?.state, "queued");
  assert.equal(control.pending, null);
  assert.equal(
    await acknowledgeSentinelIncident(
      kv as unknown as Deno.Kv,
      { incidentId: first.incidentId, attempt: 1, workflowRunId: 12345, ackNonce: ACK_ONE },
      NOW + 21,
    ),
    "duplicate",
  );
  assert.equal((await readControl(kv)).active?.id, pending.incidentId);
  await assert.rejects(
    () =>
      acknowledgeSentinelIncident(
        kv as unknown as Deno.Kv,
        { incidentId: first.incidentId, attempt: 1, workflowRunId: 99999, ackNonce: ACK_ONE },
        NOW + 22,
      ),
    SentinelIncidentAckConflict,
  );
});

Deno.test("sentinel outbox retains ambiguous delivery and advances only confirmed workflow failures", async () => {
  const kv = new CountingKv();
  await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, { randomUuid: () => UUID_ONE });
  const failedDispatch = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher: githubFetcher([], { dispatchStatus: 500 }),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(failedDispatch.status, "deferred");
  assert.equal(failedDispatch.reason, "github_workflow_dispatch_http_500");
  assert.equal((await readControl(kv)).active?.attempt, 1);
  assert.equal((await readControl(kv)).active?.state, "dispatching");

  const retryAt = NOW + 60_000;
  await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => retryAt,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  const confirmed = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => retryAt + 5 * 60_000,
    fetcher: githubFetcher([], { runConclusion: "failure" }),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(confirmed.status, "retry");
  const control = await readControl(kv);
  assert.equal(control.active?.attempt, 2);
  assert.equal(control.active?.state, "queued");
});

Deno.test("authenticated auth preflight deferral preserves the repair attempt and fences the stale run", async () => {
  const kv = new CountingKv();
  const incident = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });

  const deferral = {
    incidentId: incident.incidentId,
    attempt: 1,
    workflowRunId: 12345,
    ackNonce: ACK_ONE,
    reason: "codex_auth_preflight_failed" as const,
  };
  let deferred = false;
  const completedRunFetcher = githubFetcher([], { runConclusion: "failure" });
  const racingFetcher: typeof fetch = async (input, init) => {
    const request = new Request(input, init);
    if (!deferred && new URL(request.url).pathname.endsWith("/actions/runs/12345")) {
      assert.equal(
        await deferSentinelIncident(kv as unknown as Deno.Kv, deferral, NOW + 5 * 60_000 + 1, {
          randomAckNonce: () => ACK_TWO,
        }),
        "deferred",
      );
      deferred = true;
    }
    return await completedRunFetcher(input, init);
  };
  const stalePoll = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 5 * 60_000,
    fetcher: racingFetcher,
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(stalePoll.status, "waiting");
  let control = await readControl(kv);
  assert.equal(control.active?.attempt, 1);
  assert.equal(control.active?.state, "queued");
  assert.equal(control.active?.workflow_run_id, null);
  assert.equal(control.active?.ack_nonce, ACK_TWO);
  assert.equal(control.active?.infrastructure_deferrals, 1);
  assert.equal(
    await deferSentinelIncident(kv as unknown as Deno.Kv, deferral, NOW + 5 * 60_000 + 2),
    "deferred",
  );
  for (
    const invalid of [
      { ackNonce: "C".repeat(43) },
      { workflowRunId: 99999 },
    ]
  ) {
    await assert.rejects(
      () =>
        deferSentinelIncident(kv as unknown as Deno.Kv, {
          ...deferral,
          ...invalid,
        }, NOW + 5 * 60_000 + 3),
      SentinelIncidentDeferConflict,
    );
  }

  const redeliveryRequests: Request[] = [];
  const redelivered = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => control.active!.next_action_at_ms,
    fetcher: githubFetcher(redeliveryRequests, { runId: 12346, tokenExpiresAt: NOW + 24 * 60 * 60_000 }),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(redelivered.status, "dispatched");
  assert.equal(redelivered.attempt, 1);
  const dispatchBody = await redeliveryRequests.at(-1)!.json();
  assert.equal(dispatchBody.inputs.incident_attempt, "1");
  assert.equal(dispatchBody.inputs.incident_ack_nonce, ACK_TWO);
  control = await readControl(kv);
  assert.equal(control.active?.workflow_run_id, 12346);
});

Deno.test("infrastructure deferrals use a separate bounded budget before dead-lettering", async () => {
  const kv = new CountingKv();
  const incident = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  let now = NOW;
  for (let index = 1; index <= MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS; index += 1) {
    const runId = 20_000 + index;
    const dispatched = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
      now: () => now,
      fetcher: githubFetcher([], { runId, tokenExpiresAt: NOW + 48 * 60 * 60_000 }),
      createTimeoutSignal: () => new AbortController().signal,
    });
    assert.equal(dispatched.status, "dispatched");
    const active = (await readControl(kv)).active!;
    assert.equal(active.attempt, 1);
    assert.equal(active.workflow_run_id, runId);
    const nextNonce = String.fromCharCode(65 + index).repeat(43);
    const disposition = await deferSentinelIncident(
      kv as unknown as Deno.Kv,
      {
        incidentId: incident.incidentId,
        attempt: 1,
        workflowRunId: runId,
        ackNonce: active.ack_nonce,
        reason: "codex_auth_preflight_failed",
      },
      now + 1,
      { randomAckNonce: () => nextNonce },
    );
    assert.equal(
      disposition,
      index === MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS ? "dead_letter" : "deferred",
    );
    if (disposition === "dead_letter") {
      assert.equal(
        await deferSentinelIncident(
          kv as unknown as Deno.Kv,
          {
            incidentId: incident.incidentId,
            attempt: 1,
            workflowRunId: runId,
            ackNonce: active.ack_nonce,
            reason: "codex_auth_preflight_failed",
          },
          now + 2,
        ),
        "dead_letter",
      );
    }
    const control = await readControl(kv);
    if (index < MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS) {
      assert.equal(control.active?.attempt, 1);
      assert.equal(control.active?.infrastructure_deferrals, index);
      assert.ok(control.active!.next_action_at_ms > now + 1);
      now = control.active!.next_action_at_ms;
    } else {
      assert.equal(control.active, null);
    }
  }
  const dead = await kv.get([...SENTINEL_INCIDENT_DEAD_PREFIX, incident.incidentId]);
  assert.deepEqual(dead.value, {
    version: 1,
    incident_id: incident.incidentId,
    attempt: 1,
    workflow_run_id: 20_000 + MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS,
    conclusion: "infrastructure_deferrals_exhausted",
    infrastructure_deferrals: MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS,
    recorded_at_ms: now + 1,
  });
});

Deno.test("an accepted ambiguous dispatch is claimed once before repair work starts", async () => {
  const kv = new CountingKv();
  const incident = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  const fetcher: typeof fetch = async (input, init) => {
    const request = new Request(input, init);
    if (new URL(request.url).pathname.endsWith("/access_tokens")) {
      return Response.json(
        { token: `ghs_${"x".repeat(600)}`, expires_at: new Date(NOW + 3_600_000).toISOString() },
        { status: 201 },
      );
    }
    await claimSentinelIncidentWorkflowRun(
      kv as unknown as Deno.Kv,
      { incidentId: incident.incidentId, attempt: 1, workflowRunId: 54321, ackNonce: ACK_ONE },
      NOW + 1,
    );
    throw new Error("dispatch response was lost");
  };
  const result = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher,
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(result.status, "deferred");
  const control = await readControl(kv);
  assert.equal(control.active?.state, "dispatched");
  assert.equal(control.active?.workflow_run_id, 54321);
  assert.equal(
    await claimSentinelIncidentWorkflowRun(
      kv as unknown as Deno.Kv,
      { incidentId: incident.incidentId, attempt: 1, workflowRunId: 54321, ackNonce: ACK_ONE },
      NOW + 2,
    ),
    "duplicate",
  );
  await assert.rejects(
    () =>
      claimSentinelIncidentWorkflowRun(
        kv as unknown as Deno.Kv,
        { incidentId: incident.incidentId, attempt: 1, workflowRunId: 99999, ackNonce: ACK_ONE },
        NOW + 3,
      ),
    SentinelIncidentClaimConflict,
  );
});

Deno.test("sentinel capture events become dispatchable only after capture publication", async () => {
  const kv = new CountingKv();
  const event = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
  });
  let fetches = 0;
  const capturing = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher: () => {
      fetches += 1;
      throw new Error("capturing events must not dispatch");
    },
  });
  assert.equal(capturing.status, "idle");
  assert.equal(fetches, 0);

  const fingerprint = "c".repeat(64);
  const manifestKey: Deno.KvKey = ["uos_ai", "sentinel_replay", "v1", "manifest", NOW, fingerprint, "capture"];
  assert.equal(
    await completeSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, event, NOW + 1, {
      status: "stored",
      fingerprint,
      manifestKey,
    }),
    true,
  );
  const dispatched = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 2,
    randomAckNonce: () => ACK_ONE,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(dispatched.status, "dispatched");
  assert.equal(dispatched.incidentId, event.value.incident_id);
  const reference = await kv.get([
    ...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX,
    event.value.incident_id,
    fingerprint,
  ]);
  assert.deepEqual(reference.value, { version: 1, manifest_key: manifestKey });
  assert.equal((await kv.get(event.key)).value, null);
});

Deno.test("sentinel recovers an abandoned capture event and default UUID generation stays callable", async () => {
  const kv = new CountingKv();
  const event = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW);
  assert.match(event.value.incident_id, /^provider-[0-9a-f-]{36}$/);
  const result = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 5 * 60_000,
    randomAckNonce: () => ACK_ONE,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(result.status, "dispatched");
  assert.equal(result.incidentId, event.value.incident_id);
  assert.equal((await kv.get(event.key)).value, null);
});

Deno.test("sentinel coalescing preserves the earliest failure regardless of UUID order", async () => {
  const kv = new CountingKv();
  const later = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW + 60_000, {
    randomUuid: () => UUID_ONE,
  });
  const earlier = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => "ffffffff-ffff-4fff-bfff-ffffffffffff",
  });
  assert.equal(
    await completeSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, later, NOW + 60_001, {
      status: "unavailable",
    }),
    true,
  );
  assert.equal(
    await completeSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, earlier, NOW + 60_001, {
      status: "unavailable",
    }),
    true,
  );
  assert.equal(
    await coalesceSentinelIncidentFailureEvents(kv as unknown as Deno.Kv, NOW + 60_002, {
      randomAckNonce: () => ACK_ONE,
    }),
    2,
  );
  const control = await readControl(kv);
  assert.equal(control.active?.first_observed_at_ms, NOW);
  assert.equal(control.active?.latest_observed_at_ms, NOW + 60_000);
  assert.equal(control.active?.failure_count, 2);
});

Deno.test("fresh capturing events cannot hide a ready incident event", async () => {
  const kv = new CountingKv();
  for (let index = 1; index <= 32; index += 1) {
    await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW, {
      randomUuid: () => `00000000-0000-4000-8000-${String(index).padStart(12, "0")}`,
    });
  }
  const ready = await createSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => "ffffffff-ffff-4fff-bfff-ffffffffffff",
  });
  assert.equal(
    await completeSentinelIncidentFailureEvent(kv as unknown as Deno.Kv, ready, NOW + 1, {
      status: "unavailable",
    }),
    true,
  );
  assert.equal(
    await coalesceSentinelIncidentFailureEvents(kv as unknown as Deno.Kv, NOW + 2, {
      randomAckNonce: () => ACK_ONE,
    }),
    1,
  );
  assert.equal((await readControl(kv)).active?.id, ready.value.incident_id);
});

Deno.test("masked provider degradation creates a ready incident without replay plaintext", async () => {
  const kv = new CountingKv();
  const incidentId = await recordSentinelProviderDegradation(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
  });
  assert.equal(
    await coalesceSentinelIncidentFailureEvents(kv as unknown as Deno.Kv, NOW + 1, {
      randomAckNonce: () => ACK_ONE,
    }),
    1,
  );
  const control = await readControl(kv);
  assert.equal(control.active?.id, incidentId);
  assert.equal(control.active?.first_observed_at_ms, NOW);
  assert.equal(control.active?.failure_count, 1);
  const references = [];
  for await (const entry of kv.list({ prefix: SENTINEL_INCIDENT_CAPTURE_REF_PREFIX })) references.push(entry);
  assert.equal(references.length, 0);
});

Deno.test("sentinel rotates the ACK nonce when a successful workflow misses its bounded ACK grace", async () => {
  const kv = new CountingKv();
  const incident = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  const firstPoll = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 5 * 60_000,
    fetcher: githubFetcher([], { runConclusion: "success" }),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(firstPoll.status, "running");
  assert.equal((await readControl(kv)).active?.success_observed_at_ms, NOW + 5 * 60_000);

  const expired = await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 10 * 60_000,
    randomAckNonce: () => ACK_TWO,
    fetcher: githubFetcher([], { runConclusion: "success" }),
    createTimeoutSignal: () => new AbortController().signal,
  });
  assert.equal(expired.status, "retry");
  assert.equal(expired.incidentId, incident.incidentId);
  const control = await readControl(kv);
  assert.equal(control.active?.attempt, 2);
  assert.equal(control.active?.state, "queued");
  assert.equal(control.active?.ack_nonce, ACK_TWO);
});

Deno.test("sentinel ACK requires the delivery nonce and the recorded workflow run", async () => {
  const kv = new CountingKv();
  const incident = await recordSentinelIncidentFailure(kv as unknown as Deno.Kv, NOW, {
    randomUuid: () => UUID_ONE,
    randomAckNonce: () => ACK_ONE,
  });
  await assert.rejects(
    () =>
      acknowledgeSentinelIncident(
        kv as unknown as Deno.Kv,
        { incidentId: incident.incidentId, attempt: 1, workflowRunId: 12345, ackNonce: ACK_ONE },
        NOW + 1,
      ),
    SentinelIncidentAckConflict,
  );
  await reconcileSentinelIncidentOutbox(kv as unknown as Deno.Kv, pkcs8Pem, {
    now: () => NOW + 2,
    fetcher: githubFetcher([]),
    createTimeoutSignal: () => new AbortController().signal,
  });
  for (
    const invalid of [
      { workflowRunId: 99999, ackNonce: ACK_ONE },
      { workflowRunId: 12345, ackNonce: ACK_TWO },
    ]
  ) {
    await assert.rejects(
      () =>
        acknowledgeSentinelIncident(
          kv as unknown as Deno.Kv,
          { incidentId: incident.incidentId, attempt: 1, ...invalid },
          NOW + 3,
        ),
      SentinelIncidentAckConflict,
    );
  }
});

Deno.test("sentinel production gate requires the exact managed production timeline before reading the key", async () => {
  const valid = {
    DENO_DEPLOY_ORG_SLUG: "ubiquity-dao",
    DENO_DEPLOY_APP_SLUG: "ai-ubq-fi",
    DENO_TIMELINE: "production",
  };
  assert.equal(isSentinelProductionRuntime({ get: (name) => valid[name as keyof typeof valid] }), true);
  for (
    const entries of [
      { ...valid, DENO_DEPLOY_ORG_SLUG: "other" },
      { ...valid, DENO_DEPLOY_APP_SLUG: "p-ai-ubq-fi" },
      { ...valid, DENO_TIMELINE: "git-branch/feature" },
    ]
  ) {
    const reads: string[] = [];
    let fetches = 0;
    const result = await reconcileSentinelIncidentOutboxFromEnvironment({
      kv: new CountingKv() as unknown as Deno.Kv,
      environment: {
        get(name) {
          reads.push(name);
          return entries[name as keyof typeof entries];
        },
      },
      fetcher: () => {
        fetches += 1;
        throw new Error("must not fetch");
      },
    });
    assert.equal(result.status, "idle");
    assert.equal(reads.includes("SENTINEL_GITHUB_APP_PRIVATE_KEY"), false);
    assert.equal(fetches, 0);
  }
});

Deno.test("sentinel incident workflow endpoints are production-only and accept strict authenticated payloads", async () => {
  const body = {
    incident_id: `provider-${UUID_ONE}`,
    attempt: 1,
    workflow_run_id: 12345,
    ack_nonce: ACK_ONE,
  };
  const hidden = await handleAdminSentinelIncidentAck(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/ack", {
      method: "POST",
      body: JSON.stringify(body),
    }),
    { isProduction: () => false },
  );
  assert.equal(hidden.status, 404);
  let claimed: unknown = null;
  const claim = await handleAdminSentinelIncidentClaim(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/claim", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    }),
    {
      isProduction: () => true,
      getKv: () => Promise.resolve({} as Deno.Kv),
      claim: (_kv, input) => {
        claimed = input;
        return Promise.resolve("claimed");
      },
    },
  );
  assert.equal(claim.status, 204);
  assert.deepEqual(claimed, {
    incidentId: body.incident_id,
    attempt: 1,
    workflowRunId: 12345,
    ackNonce: ACK_ONE,
  });
  let observed: unknown = null;
  const accepted = await handleAdminSentinelIncidentAck(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/ack", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    }),
    {
      isProduction: () => true,
      getKv: () => Promise.resolve({} as Deno.Kv),
      acknowledge: (_kv, input) => {
        observed = input;
        return Promise.resolve("acknowledged");
      },
    },
  );
  assert.equal(accepted.status, 204);
  assert.deepEqual(observed, {
    incidentId: body.incident_id,
    attempt: 1,
    workflowRunId: 12345,
    ackNonce: ACK_ONE,
  });
  let deferred: unknown = null;
  const deferBody = { ...body, reason: "codex_auth_preflight_failed" };
  const acceptedDeferral = await handleAdminSentinelIncidentDefer(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/defer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(deferBody),
    }),
    {
      isProduction: () => true,
      getKv: () => Promise.resolve({} as Deno.Kv),
      defer: (_kv, input) => {
        deferred = input;
        return Promise.resolve("deferred");
      },
    },
  );
  assert.equal(acceptedDeferral.status, 204);
  assert.equal(acceptedDeferral.headers.get("X-Sentinel-Incident-Disposition"), "deferred");
  assert.deepEqual(deferred, {
    incidentId: body.incident_id,
    attempt: 1,
    workflowRunId: 12345,
    ackNonce: ACK_ONE,
    reason: "codex_auth_preflight_failed",
  });
  const deadLetterDeferral = await handleAdminSentinelIncidentDefer(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/defer", {
      method: "POST",
      body: JSON.stringify({ ...body, reason: "sentinel_infrastructure_preflight_failed" }),
    }),
    {
      isProduction: () => true,
      getKv: () => Promise.resolve({} as Deno.Kv),
      defer: () => Promise.resolve("dead_letter"),
    },
  );
  assert.equal(deadLetterDeferral.status, 204);
  assert.equal(deadLetterDeferral.headers.get("X-Sentinel-Incident-Disposition"), "dead_letter");
  const invalidDeferral = await handleAdminSentinelIncidentDefer(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/defer", {
      method: "POST",
      body: JSON.stringify({ ...deferBody, reason: "workflow_failed" }),
    }),
    { isProduction: () => true },
  );
  assert.equal(invalidDeferral.status, 400);
  const staleDeferral = await handleAdminSentinelIncidentDefer(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/defer", {
      method: "POST",
      body: JSON.stringify(deferBody),
    }),
    {
      isProduction: () => true,
      getKv: () => Promise.resolve({} as Deno.Kv),
      defer: () => Promise.reject(new SentinelIncidentDeferConflict()),
    },
  );
  assert.equal(staleDeferral.status, 409);

  const { default: routedHandler } = await import("../src/handler.ts");
  const unauthenticatedDeferral = await routedHandler(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/defer", {
      method: "POST",
      body: JSON.stringify(deferBody),
    }),
  );
  assert.equal(unauthenticatedDeferral.status, 401);
  const invalid = await handleAdminSentinelIncidentAck(
    new Request("https://ai.ubq.fi/admin/sentinel/incidents/ack", {
      method: "POST",
      body: JSON.stringify({ ...body, extra: true }),
    }),
    { isProduction: () => true },
  );
  assert.equal(invalid.status, 400);
});
