import assert from "node:assert/strict";
import { apiKeyHashKey } from "../src/api_keys.ts";
import {
  API_KEY_USAGE_V3_RESERVATION_LEASE_MS,
  type ApiKeyPolicy,
  apiKeyPolicyFromHashRecord,
  type ApiKeyUsageReservation,
  apiKeyUsageV3RequestKey,
  apiKeyUsageV3WindowKey,
  reserveApiKeyUsageV3,
} from "../src/api_key_policy.ts";
import type { ApiKeyHashRecord, ApiKeyUsageRequestV3, ApiKeyUsageWindowV3 } from "../src/types.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const storedValue = <T>(kv: CountingKv, key: Deno.KvKey): T | null =>
  (kv.entries.get(JSON.stringify(key))?.value as T | undefined) ?? null;

const setupPolicy = (
  id: string,
  usageLimitRequests: number,
  nowMs = Date.now(),
): { kv: CountingKv; policy: ApiKeyPolicy } => {
  const tokenHash = `dispatch-reuse-${id}`;
  const record: ApiKeyHashRecord = {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
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
  if (!policy) throw new Error("test API key policy must be valid");
  const kv = new CountingKv();
  kv.seed(apiKeyHashKey(tokenHash), record);
  return { kv, policy };
};

const reserve = async (
  kv: CountingKv,
  policy: ApiKeyPolicy,
  requestId: string,
  nowMs = Date.now(),
): Promise<ApiKeyUsageReservation> => {
  const decision = await reserveApiKeyUsageV3(policy, requestId, "responses", {
    kv: kv as unknown as Deno.Kv,
    nowMs,
  });
  if (!decision.ok) throw new Error(`unexpected admission failure: ${decision.response.status}`);
  return decision.reservation;
};

const windowFor = (kv: CountingKv, policy: ApiKeyPolicy): ApiKeyUsageWindowV3 => {
  const window = storedValue<ApiKeyUsageWindowV3>(kv, apiKeyUsageV3WindowKey(policy));
  if (!window) throw new Error("expected V3 aggregate window");
  return window;
};

const requestFor = (kv: CountingKv, policy: ApiKeyPolicy, requestId: string): ApiKeyUsageRequestV3 => {
  const request = storedValue<ApiKeyUsageRequestV3>(kv, apiKeyUsageV3RequestKey(policy, requestId));
  if (!request) throw new Error("expected V3 request row");
  return request;
};

Deno.test("V3 local dispatch state skips only the redundant post-dispatch release reads", async () => {
  const { kv, policy } = setupPolicy("normal-dispatch", 2);
  const finish = kv.beginMeasurement({ authKind: "bounded_api_key", outcome: "post_dispatch_release" });
  try {
    const reservation = await reserve(kv, policy, "normal-dispatch-request");
    const dispatch = await reservation.beforeProviderDispatch("chatgpt_codex");
    if (!dispatch) throw new Error("first dispatch must create a transport context");
    dispatch.markTransportStarted();

    const beforeRelease = kv.commands.length;
    await reservation.release("provider_http_failure");
    assert.equal(kv.commands.length, beforeRelease, "a locally dispatched reservation must not reread for completion");
  } finally {
    finish();
  }

  const [budget] = kv.budgets();
  assert.deepEqual(
    {
      commands: budget?.commands,
      read_commands: budget?.read_commands,
      write_mutations: budget?.write_mutations,
      atomic_commits: budget?.atomic_commits,
      atomic_checks: budget?.atomic_checks,
      atomic_mutations: budget?.atomic_mutations,
    },
    {
      commands: 7,
      read_commands: 5,
      write_mutations: 4,
      atomic_commits: 2,
      atomic_checks: 5,
      atomic_mutations: 4,
    },
  );
  assert.deepEqual(
    {
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
      state: requestFor(kv, policy, "normal-dispatch-request").state,
    },
    { committed: 1, reserved: 0, state: "dispatched" },
  );
});

Deno.test("V3 pre-transport cancellation still compensates before the local completion no-op", async () => {
  const { kv, policy } = setupPolicy("pre-transport-cancellation", 1);
  const reservation = await reserve(kv, policy, "pre-transport-cancellation-request");
  const dispatch = await reservation.beforeProviderDispatch("chatgpt_codex");
  if (!dispatch) throw new Error("first dispatch must create a transport context");

  const beforeCancellation = kv.commands.length;
  await dispatch.cancelBeforeTransport();
  const afterCancellation = kv.commands.length;
  assert.equal(
    afterCancellation - beforeCancellation,
    3,
    "cancellation must retain two reads and its CAS compensation",
  );

  await reservation.release();
  assert.equal(kv.commands.length, afterCancellation, "completion must not issue a second settled-state reread");
  assert.deepEqual(
    {
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
      state: requestFor(kv, policy, "pre-transport-cancellation-request").state,
    },
    { committed: 0, reserved: 0, state: "released" },
  );
});

Deno.test("V3 retry and upstream failure retain one dispatch while completion stays read-free", async () => {
  const { kv, policy } = setupPolicy("retry-and-upstream-failure", 2);
  const reservation = await reserve(kv, policy, "retry-and-upstream-failure-request");
  const firstDispatch = await reservation.beforeProviderDispatch("chatgpt_codex");
  if (!firstDispatch) throw new Error("first dispatch must create a transport context");
  firstDispatch.markTransportStarted();

  const beforeRetry = kv.commands.length;
  const retryDispatch = await reservation.beforeProviderDispatch("yunwu");
  assert.equal(retryDispatch, undefined, "a retry must not commit another dispatch");
  assert.equal(
    kv.commands.length - beforeRetry,
    2,
    "the existing retry ledger verification remains strongly read-backed",
  );

  const beforeCompletion = kv.commands.length;
  await reservation.release("provider_http_failure");
  assert.equal(kv.commands.length, beforeCompletion, "a dispatched upstream failure must not add completion reads");
  assert.deepEqual(
    {
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
      provider: requestFor(kv, policy, "retry-and-upstream-failure-request").provider,
    },
    { committed: 1, reserved: 0, provider: "chatgpt_codex" },
  );
});

Deno.test("V3 non-dispatch paths retain the durable release CAS", async () => {
  const { kv, policy } = setupPolicy("non-dispatch", 1);
  const reservation = await reserve(kv, policy, "non-dispatch-request");
  const beforeRelease = kv.commands.length;

  await reservation.release("validation_failed");

  assert.equal(kv.commands.length - beforeRelease, 3, "an undispatched reservation must still read and CAS-release");
  assert.deepEqual(
    {
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
      state: requestFor(kv, policy, "non-dispatch-request").state,
      reason: requestFor(kv, policy, "non-dispatch-request").release_reason,
    },
    { committed: 0, reserved: 0, state: "released", reason: "validation_failed" },
  );
});

Deno.test("V3 duplicate admission and concurrent bounded admission preserve one reservation and dispatch", async () => {
  const { kv, policy } = setupPolicy("idempotency-and-concurrency", 1);
  const first = await reserve(kv, policy, "same-request");
  const duplicate = await reserve(kv, policy, "same-request");
  assert.equal(windowFor(kv, policy).reserved_requests, 1, "the same request id must not reserve quota twice");

  const dispatch = await duplicate.beforeProviderDispatch("cerebras");
  if (!dispatch) throw new Error("duplicate admission must still dispatch the one durable row");
  dispatch.markTransportStarted();
  await first.release();
  assert.equal(windowFor(kv, policy).committed_requests, 1);

  const { kv: concurrentKv, policy: concurrentPolicy } = setupPolicy("concurrent-admission", 1);
  const decisions = await Promise.all(
    Array.from({ length: 8 }, (_, index) =>
      reserveApiKeyUsageV3(concurrentPolicy, `concurrent-${index}`, "responses", {
        kv: concurrentKv as unknown as Deno.Kv,
      })),
  );
  const admitted = decisions.filter((decision) => decision.ok);
  assert.equal(admitted.length, 1, "only one bounded request may reserve the last slot");
  const winner = admitted[0];
  if (!winner?.ok) throw new Error("expected a concurrent admission winner");
  const winnerDispatch = await winner.reservation.beforeProviderDispatch("voyage");
  if (!winnerDispatch) throw new Error("the winning reservation must dispatch");
  winnerDispatch.markTransportStarted();
  await winner.reservation.release();
  assert.deepEqual(
    {
      committed: windowFor(concurrentKv, concurrentPolicy).committed_requests,
      reserved: windowFor(concurrentKv, concurrentPolicy).reserved_requests,
    },
    { committed: 1, reserved: 0 },
  );
});

Deno.test("V3 abandoned reservations remain reclaimable after a simulated process crash", async () => {
  const nowMs = Date.now();
  const { kv, policy } = setupPolicy("crash-reclaim", 1, nowMs);
  await reserve(kv, policy, "abandoned-request", nowMs);

  const replacement = await reserve(
    kv,
    policy,
    "replacement-request",
    nowMs + API_KEY_USAGE_V3_RESERVATION_LEASE_MS + 1,
  );

  assert.deepEqual(
    {
      abandonedState: requestFor(kv, policy, "abandoned-request").state,
      abandonedReason: requestFor(kv, policy, "abandoned-request").release_reason,
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
    },
    { abandonedState: "released", abandonedReason: "lease_expired", committed: 0, reserved: 1 },
  );
  await replacement.release();
  assert.deepEqual(
    {
      committed: windowFor(kv, policy).committed_requests,
      reserved: windowFor(kv, policy).reserved_requests,
    },
    { committed: 0, reserved: 0 },
  );
});
