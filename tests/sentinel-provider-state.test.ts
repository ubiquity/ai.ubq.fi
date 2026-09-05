import assert from "node:assert/strict";
import {
  createGitHubSentinelBootstrapState,
  createGitHubSentinelProviderState,
  parseSentinelBootstrapStateDocument,
  SENTINEL_BOOTSTRAP_STATE_PATH,
  SENTINEL_PROVIDER_STATE_PATH,
} from "../scripts/sentinel/bootstrap/github-store.ts";
import { SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH } from "../scripts/sentinel/bootstrap/policy.ts";
import {
  parseSentinelProviderStateDocument,
  type SentinelProviderStateDocumentV1,
  type SentinelProviderTransactionV1,
} from "../scripts/sentinel/bootstrap/provider-state.ts";
import type { SentinelBootstrapReleaseRecordV1 } from "../scripts/sentinel/bootstrap/contracts.ts";

type Fixture = Record<string, unknown>;

const SHA_A = "a".repeat(40);
const SHA_B = "b".repeat(40);
const SHA_C = "c".repeat(40);
const SHA_V = "d".repeat(40);
const D1 = "1".repeat(64);
const D2 = "2".repeat(64);
const D3 = "3".repeat(64);
const VERIFIED_A = "2026-09-04T09:00:00.000Z";
const VERIFIED_B = "2026-09-04T10:00:00.000Z";
const CREATED_AT = "2026-09-04T11:00:00.000Z";
const INTENT = "2026-09-04T12:00:00.000Z";
const DEADLINE = "2026-09-04T12:30:00.000Z";
const OBSERVED = "2026-09-04T12:40:00.000Z";
const ROLLBACK_INTENT = "2026-09-04T13:00:00.000Z";
const RESTORED_AT = "2026-09-04T13:10:00.000Z";
const ROLLBACK_RESULT_AT = "2026-09-04T13:05:00.000Z";
const STOPPED_AT = "2026-09-04T11:05:00.000Z";

const attestation = (overrides: Record<string, unknown> = {}): Fixture => ({
  git_sha: SHA_A,
  revision_id: "rev-a",
  configuration_digest: D1,
  validator_sha: SHA_V,
  corpus_digest: D2,
  verified_at: VERIFIED_A,
  identity_ref: "github:attestation-a",
  inference_ref: "codex:run-a",
  ...overrides,
});

const previousAttestation = (): Fixture => attestation();

const candidateAttestation = (): Fixture =>
  attestation({
    git_sha: SHA_B,
    revision_id: "rev-b",
    verified_at: VERIFIED_B,
    identity_ref: "github:attestation-b",
    inference_ref: "codex:run-b",
  });

const restorationAttestation = (): Fixture => attestation({ verified_at: RESTORED_AT });

const executor = (overrides: Record<string, unknown> = {}): Fixture => ({
  repository: "ubiquity/ai.ubq.fi",
  workflow_path: ".github/workflows/provider-sentinel.yml",
  run_id: 42,
  run_attempt: 1,
  ...overrides,
});

const zeroObservation = (): Fixture => ({
  last_observed_at: null,
  samples: 0,
  consecutive_liveness_failures: 0,
  consecutive_inference_failures: 0,
  invariant_id: null,
  consecutive_invariant_failures: 0,
});

const acknowledged = (): Fixture => ({
  kind: "acknowledged",
  http_status: 204,
  observed_at: "2026-09-04T12:10:00.000Z",
  evidence_ref: "github:promotion-ok",
});

const ambiguous = (): Fixture => ({
  kind: "ambiguous",
  http_status: null,
  observed_at: "2026-09-04T12:10:00.000Z",
  evidence_ref: "github:promotion-ambiguous",
});

const preparedTransaction = (overrides: Record<string, unknown> = {}): Fixture => ({
  id: "tx-1",
  fence_generation: 2,
  phase: "prepared",
  previous: previousAttestation(),
  candidate: candidateAttestation(),
  expected_merged_sha: SHA_B,
  executor: executor(),
  retired_executor: null,
  previous_transaction_commit: null,
  created_at: CREATED_AT,
  promotion_intent_at: null,
  promotion_result: null,
  observation_deadline_at: null,
  observation: zeroObservation(),
  route: null,
  decision: null,
  reason: null,
  rollback_intent_at: null,
  rollback_result: null,
  restoration: null,
  ...overrides,
});

const promotionPending = (overrides: Record<string, unknown> = {}): Fixture =>
  preparedTransaction({
    phase: "promotion_pending",
    promotion_intent_at: INTENT,
    observation_deadline_at: DEADLINE,
    ...overrides,
  });

const observing = (overrides: Record<string, unknown> = {}): Fixture =>
  promotionPending({
    phase: "observing",
    promotion_result: acknowledged(),
    ...overrides,
  });

const rollbackPending = (overrides: Record<string, unknown> = {}): Fixture =>
  observing({
    phase: "rollback_pending",
    promotion_result: ambiguous(),
    decision: "rollback",
    reason: "candidate_unhealthy",
    rollback_intent_at: ROLLBACK_INTENT,
    ...overrides,
  });

const rollbackPendingVerification = (overrides: Record<string, unknown> = {}): Fixture =>
  rollbackPending({
    phase: "rollback_pending_verification",
    rollback_result: { ...acknowledged(), observed_at: "2026-09-04T13:05:00.000Z" },
    ...overrides,
  });

const keptTransaction = (overrides: Record<string, unknown> = {}): Fixture =>
  observing({
    phase: "kept",
    decision: "keep",
    reason: "promotion_verified",
    observation: {
      last_observed_at: OBSERVED,
      samples: 3,
      consecutive_liveness_failures: 0,
      consecutive_inference_failures: 0,
      invariant_id: null,
      consecutive_invariant_failures: 0,
    },
    route: { revision_id: "rev-b", observed_at: OBSERVED, evidence_ref: "github:route-candidate" },
    ...overrides,
  });

const rolledBackTransaction = (overrides: Record<string, unknown> = {}): Fixture =>
  rollbackPendingVerification({
    phase: "rolled_back",
    route: { revision_id: "rev-a", observed_at: ROLLBACK_RESULT_AT, evidence_ref: "github:route-previous" },
    restoration: restorationAttestation(),
    ...overrides,
  });

const blockedTransaction = (overrides: Record<string, unknown> = {}): Fixture =>
  observing({
    phase: "blocked",
    decision: "dependency_failure",
    reason: "candidate_unhealthy",
    ...overrides,
  });

const appState = (overrides: Record<string, unknown> = {}): Fixture => ({
  app: "ai-ubq-fi",
  healthy: previousAttestation(),
  transaction: null,
  ...overrides,
});

const stateDocument = (
  overrides: Record<string, unknown> = {},
  generation: number = 2,
): Fixture => ({
  schema_version: 1,
  generation,
  applications: [appState()],
  ...overrides,
});

const parseOk = (value: unknown): SentinelProviderStateDocumentV1 => parseSentinelProviderStateDocument(value);

const messageOf = (run: () => unknown): string => {
  try {
    run();
  } catch (error) {
    return String(error);
  }
  return "";
};

const assertInvalid = (candidate: unknown, pattern: RegExp): void => {
  assert.throws(() => parseSentinelProviderStateDocument(candidate), pattern);
};

Deno.test("a valid prepared document parses to a normalized, deeply frozen document", () => {
  const parsed = parseOk(stateDocument({
    applications: [
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: preparedTransaction() },
      { app: "ai-ubq-fi", healthy: null, transaction: null },
    ],
  }));
  assert.deepEqual(
    parsed.applications.map((entry) => entry.app),
    ["ai-ubq-fi", "p-ai-ubq-fi"],
    "applications must normalize to fixed lexical app order",
  );
  assert.equal(parsed.generation, 2);
  const transaction = parsed.applications[1]!.transaction!;
  assert.equal(transaction.phase, "prepared");
  assert.deepEqual(transaction.previous, parsed.applications[1]!.healthy);
  assert.ok(Object.isFrozen(parsed));
  assert.ok(Object.isFrozen(parsed.applications));
  assert.ok(Object.isFrozen(parsed.applications[0]!));
  assert.ok(Object.isFrozen(parsed.applications[1]!.healthy!));
  assert.ok(Object.isFrozen(transaction));
  assert.ok(Object.isFrozen(transaction.previous));
  assert.ok(Object.isFrozen(transaction.observation));
  assert.ok(Object.isFrozen(transaction.executor));
  assert.throws(() => {
    (parsed as { generation: number }).generation = 42;
  }, TypeError);
  assert.throws(() => {
    (transaction as { id: string }).id = "changed";
  }, TypeError);
  assert.deepEqual(
    parseOk(JSON.parse(JSON.stringify(parsed))),
    parsed,
    "serializing and reparsing must round-trip exactly",
  );
});

Deno.test("every non-prepared phase accepts a structurally valid transaction", () => {
  const phaseFixtures: readonly [string, Fixture, Fixture][] = [
    ["promotion_pending", promotionPending(), previousAttestation()],
    ["observing", observing(), previousAttestation()],
    ["rollback_pending", rollbackPending(), previousAttestation()],
    ["rollback_pending_verification", rollbackPendingVerification(), previousAttestation()],
    ["kept", keptTransaction(), candidateAttestation()],
    ["rolled_back", rolledBackTransaction(), restorationAttestation()],
    ["blocked", blockedTransaction(), previousAttestation()],
  ];
  for (const [name, transaction, healthy] of phaseFixtures) {
    const parsed = parseOk(stateDocument({ applications: [{ app: "ai-ubq-fi", healthy, transaction }] }));
    assert.equal(parsed.applications[0]!.transaction!.phase, name);
  }
  // A promotion result may also be ambiguous while observing.
  const ambiguousObserving = parseOk(stateDocument({
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: observing({
        promotion_result: ambiguous(),
      }),
    }],
  }));
  assert.equal(ambiguousObserving.applications[0]!.transaction!.promotion_result!.kind, "ambiguous");
});

Deno.test("a retired executor is structurally distinct evidence, never authoritative termination", () => {
  // Fixture-only test: this storage unit binds exact identity; it does not
  // claim authoritative executor termination from a fixture.
  const transaction = preparedTransaction({
    retired_executor: {
      executor: executor({ run_id: 43 }),
      conclusion: "stale",
      observed_at: STOPPED_AT,
      evidence_ref: "github:stop-evidence",
    },
  });
  const parsed = parseOk(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction }] }),
  );
  const retired = parsed.applications[0]!.transaction!.retired_executor!;
  assert.equal(retired.executor.run_id, 43);
  assert.equal(retired.conclusion, "stale");
  assert.equal(retired.observed_at, STOPPED_AT);
  // Same executor or evidence before creation is rejected.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          retired_executor: {
            executor: executor({ run_id: 42 }),
            conclusion: "stale",
            observed_at: STOPPED_AT,
            evidence_ref: "github:stop-evidence",
          },
        }),
      }],
    }),
    /retired_executor must differ/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          retired_executor: {
            executor: executor({ run_id: 43 }),
            conclusion: "stale",
            observed_at: "2026-09-04T10:59:59.999Z", // strictly before created_at
            evidence_ref: "github:stop-evidence",
          },
        }),
      }],
    }),
    /retired_executor\.observed_at/u,
  );
});

Deno.test("malformed schema documents are rejected with safe field labels", () => {
  assertInvalid(stateDocument({ extra: 1 }), /provider_state contains an unrecognized field/u);
  const hostileKey = stateDocument({
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null, "injected|key": true }],
  });
  const message = messageOf(() => parseSentinelProviderStateDocument(hostileKey));
  assert.match(message, /applications\[0\] contains an unrecognized field/u);
  assert.ok(!message.includes("injected"), "the unknown key must never be echoed into an error");
});

Deno.test("unknown and missing keys are rejected at every object layer", () => {
  const withUnknownAppKey = stateDocument({
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null, bogus: true }],
  });
  assertInvalid(withUnknownAppKey, /applications\[0\] contains an unrecognized field/u);
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          phase_extra: true,
        }),
      }],
    }),
    /transaction contains an unrecognized field/u,
  );
  assertInvalid(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: attestation({ bogus: 1 }), transaction: null }] }),
    /healthy contains an unrecognized field/u,
  );
  // An inherited key can never satisfy a required field.
  const inherited = attestation() as Fixture;
  delete inherited.git_sha;
  const withInheritedGitSha = Object.create(
    { git_sha: SHA_A },
    Object.getOwnPropertyDescriptors(inherited),
  ) as Fixture;
  assertInvalid(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: withInheritedGitSha, transaction: null }] }),
    /healthy\.git_sha is missing/u,
  );
  const missingObservation = preparedTransaction() as Fixture;
  delete missingObservation.observation;
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: missingObservation }],
    }),
    /transaction\.observation is missing/u,
  );
  const missingSchema = stateDocument() as Fixture;
  delete missingSchema.schema_version;
  assertInvalid(missingSchema, /provider_state\.schema_version is missing/u);
});

Deno.test("invalid nulls, floats, and overflow integers are rejected", () => {
  assertInvalid(
    stateDocument({ generation: 1.5 }),
    /generation must be a positive safe integer/u,
  );
  assertInvalid(
    stateDocument({ generation: Number.MAX_SAFE_INTEGER + 2 }),
    /generation must be a positive safe integer/u,
  );
  assertInvalid(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: null, transaction: preparedTransaction() }] }),
    /transaction requires an initialized healthy attestation/u,
  );
  assertInvalid(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: "null", transaction: null }] }),
    /healthy must be an object/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          observation: { ...zeroObservation(), samples: 1.5 },
        }),
      }],
    }),
    /samples must be a nonnegative safe integer/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          executor: executor({ run_id: 1.5 }),
        }),
      }],
    }),
    /run_id must be a positive safe integer/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          fence_generation: 3,
        }),
      }],
    }),
    /fence_generation cannot exceed the document generation/u,
  );
});

Deno.test("malformed strings and times are rejected", () => {
  assertInvalid(stateDocument({ generation: "1" }), /generation must be a positive safe integer/u);
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ git_sha: "A".repeat(40) }), transaction: null }],
    }),
    /healthy\.git_sha must be a lowercase 40-character Git SHA/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ revision_id: "-rev-a" }), transaction: null }],
    }),
    /revision_id must be a lowercase DNS label/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ revision_id: "Rev-a" }), transaction: null }],
    }),
    /revision_id must be a lowercase DNS label/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ revision_id: "a".repeat(64) }), transaction: null }],
    }),
    /revision_id must be a lowercase DNS label/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ configuration_digest: "x".repeat(64) }),
        transaction: null,
      }],
    }),
    /configuration_digest must be a lowercase 64-character digest/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ verified_at: "2026-09-04T09:00:00Z" }),
        transaction: null,
      }],
    }),
    /verified_at must be an ISO UTC timestamp/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ verified_at: "2026-09-04T09:00:00.00Z" }),
        transaction: null,
      }],
    }),
    /verified_at must be an ISO UTC timestamp/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ verified_at: "2026-02-30T09:00:00.000Z" }),
        transaction: null,
      }],
    }),
    /verified_at must be a canonical UTC timestamp/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ identity_ref: "github" }), transaction: null }],
    }),
    /identity_ref must be a bounded nonempty reference/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ identity_ref: "Github:thing" }), transaction: null }],
    }),
    /identity_ref must be a bounded nonempty reference/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ identity_ref: `g${"g".repeat(510)}:x` }),
        transaction: null,
      }],
    }),
    /identity_ref must be a bounded nonempty reference/u,
  );
  parseOk(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: attestation({ identity_ref: `g${"g".repeat(509)}:x` }),
        transaction: null,
      }],
    }),
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          reason: "not valid reason!",
          phase: "kept",
          decision: "keep",
        }),
      }],
    }),
    /reason must be an identifier/u,
  );
  assertInvalid(
    stateDocument({ applications: [{ app: "p-other", healthy: previousAttestation(), transaction: null }] }),
    /applications\[0\]\.app must be a known application/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          executor: executor({ repository: "other/owner-repo" }),
        }),
      }],
    }),
    /repository must be ubiquity\/ai\.ubq\.fi/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          executor: executor({ workflow_path: ".github/workflows/Bad.yml" }),
        }),
      }],
    }),
    /workflow_path must be a safe workflow path/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          executor: executor({ workflow_path: ".github/workflows/../../evil.yml" }),
        }),
      }],
    }),
    /workflow_path must be a safe workflow path/u,
  );
});

Deno.test("identity inconsistency and transaction wiring are rejected", () => {
  // previous/candidate must differ in both SHA and revision.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          candidate: attestation({ git_sha: SHA_A }),
        }),
      }],
    }),
    /must differ in both SHA and revision/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          candidate: attestation({ revision_id: "rev-a" }),
        }),
      }],
    }),
    /must differ in both SHA and revision/u,
  );
  // Config/validator/corpus must match between candidate and previous.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          candidate: attestation({
            git_sha: SHA_B,
            revision_id: "rev-b",
            verified_at: VERIFIED_B,
            configuration_digest: D3,
          }),
        }),
      }],
    }),
    /candidate config\/validator\/corpus must match previous/u,
  );
  // expected_merged_sha must equal candidate.git_sha.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          expected_merged_sha: SHA_C,
        }),
      }],
    }),
    /expected_merged_sha must equal the candidate Git SHA/u,
  );
  // Attestations cannot be verified after creation.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          previous: attestation({ verified_at: INTENT }),
        }),
      }],
    }),
    /attestations cannot be verified after creation/u,
  );
});

Deno.test("phase combination rules are enforced", () => {
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          promotion_intent_at: INTENT,
        }),
      }],
    }),
    /prepared phase must not carry promotion intent/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          observation: {
            last_observed_at: OBSERVED,
            samples: 1,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /prepared phase must not carry a route or observation samples/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: promotionPending({
          promotion_result: acknowledged(),
        }),
      }],
    }),
    /promotion_pending only permits a null or ambiguous promotion_result/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          promotion_result: null,
        }),
      }],
    }),
    /observing requires a promotion_result/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: rollbackPending({
          rollback_result: { ...acknowledged(), observed_at: "2026-09-04T13:05:00.000Z" },
        }),
      }],
    }),
    /rollback_pending only permits a null or ambiguous rollback_result/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: rollbackPendingVerification({
          rollback_result: { ...ambiguous(), observed_at: "2026-09-04T13:05:00.000Z" },
        }),
      }],
    }),
    /rollback_pending_verification requires an acknowledged rollback_result/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: rollbackPending({
          reason: null,
        }),
      }],
    }),
    /reason is required for a decision/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: candidateAttestation(),
        transaction: keptTransaction({
          observation: {
            last_observed_at: OBSERVED,
            samples: 3,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: "inv-1",
            consecutive_invariant_failures: 1,
          },
        }),
      }],
    }),
    /kept requires zero consecutive failures/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: candidateAttestation(),
        transaction: keptTransaction({
          route: { revision_id: "rev-a", observed_at: OBSERVED, evidence_ref: "github:route-previous" },
        }),
      }],
    }),
    /kept requires the exact candidate route/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: candidateAttestation(),
        transaction: keptTransaction({
          observation: {
            last_observed_at: "2026-09-04T12:29:59.999Z",
            samples: 1,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /kept requires an observation at or after the deadline/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: restorationAttestation(),
        transaction: rolledBackTransaction({
          route: { revision_id: "rev-b", observed_at: ROLLBACK_RESULT_AT, evidence_ref: "github:route-candidate" },
        }),
      }],
    }),
    /rolled_back requires the exact previous route/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: blockedTransaction({
          decision: "keep",
        }),
      }],
    }),
    /blocked requires a dependency_failure or ownership_unresolved decision/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          decision: "keep",
          reason: null,
        }),
      }],
    }),
    /reason is required for a decision/u,
  );
  // Route must identify candidate or previous; a third identity is never adopted.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          route: { revision_id: "rev-c", observed_at: OBSERVED, evidence_ref: "github:route-third" },
        }),
      }],
    }),
    /route must identify the candidate or previous revision/u,
  );
  // healthy must exactly equal previous (all non-terminal phases) and the
  // phase-specific target for kept/rolled_back.
  assertInvalid(
    stateDocument({ applications: [{ app: "ai-ubq-fi", healthy: candidateAttestation(), transaction: observing() }] }),
    /healthy must exactly equal the previous attestation/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: keptTransaction() }],
    }),
    /healthy must exactly equal the kept candidate/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: candidateAttestation(), transaction: rolledBackTransaction() }],
    }),
    /healthy must exactly equal the restoration/u,
  );
});

Deno.test("observation, deadline, intent, and result timestamp rules are enforced", () => {
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation_deadline_at: "2026-09-04T12:31:00.000Z",
        }),
      }],
    }),
    /observation_deadline_at must be exactly 30 minutes after promotion_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          promotion_intent_at: "2026-09-04T10:00:00.000Z",
        }),
      }],
    }),
    /requires promotion_intent_at at or after created_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            last_observed_at: "2026-09-04T11:30:00.000Z",
            samples: 1,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /observation cannot precede promotion_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          promotion_result: { ...acknowledged(), observed_at: "2026-09-04T11:30:00.000Z" },
        }),
      }],
    }),
    /promotion_result cannot precede promotion_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          rollback_intent_at: ROLLBACK_INTENT,
          rollback_result: acknowledged(),
          decision: "rollback",
          reason: "candidate_unhealthy",
          phase: "rollback_pending_verification",
        }),
      }],
    }),
    // intent is before promotion intent only if INTENT is later; keep the
    // pre-existing wiring check: rollback requires promotion intent (present).
    /rollback_result cannot precede rollback_intent_at/u,
  );
  // Restoration and rollback results require rollback intent; rollback intent
  // requires promotion intent.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          restoration: attestation({ verified_at: "2026-09-04T12:50:00.000Z" }),
          rollback_result: { ...acknowledged(), observed_at: "2026-09-04T13:05:00.000Z" },
          decision: "rollback",
          reason: "candidate_unhealthy",
          phase: "rolled_back",
          rollback_intent_at: ROLLBACK_INTENT,
          route: { revision_id: "rev-a", observed_at: ROLLBACK_RESULT_AT, evidence_ref: "github:route-previous" },
        }),
      }],
    }),
    /restoration cannot be verified before rollback_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          rollback_intent_at: ROLLBACK_INTENT,
        }),
      }],
    }),
    /rollback_intent_at requires promotion_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          rollback_result: acknowledged(),
          decision: "rollback",
          reason: "candidate_unhealthy",
          phase: "rollback_pending_verification",
          rollback_intent_at: null,
        }),
      }],
    }),
    /rollback_result requires rollback_intent_at/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          restoration: restorationAttestation(),
          decision: "rollback",
          reason: "candidate_unhealthy",
          phase: "rolled_back",
          rollback_intent_at: null,
          rollback_result: null,
          route: { revision_id: "rev-a", observed_at: OBSERVED, evidence_ref: "github:route-previous" },
        }),
      }],
    }),
    /restoration requires rollback_intent_at/u,
  );
  // Restoration must match the previous identity.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: restorationAttestation(),
        transaction: rolledBackTransaction({
          restoration: attestation({ git_sha: SHA_C, verified_at: RESTORED_AT }),
        }),
      }],
    }),
    /restoration must match the previous identity/u,
  );
});

Deno.test("observation counters are bounded by samples and identities", () => {
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            last_observed_at: OBSERVED,
            samples: 2,
            consecutive_liveness_failures: 3,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /failure counters cannot exceed samples/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            last_observed_at: null,
            samples: 0,
            consecutive_liveness_failures: 1,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /must be empty when samples is 0/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            last_observed_at: null,
            samples: 1,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 0,
          },
        }),
      }],
    }),
    /last_observed_at is required when samples is positive/u,
  );
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            last_observed_at: OBSERVED,
            samples: 1,
            consecutive_liveness_failures: 0,
            consecutive_inference_failures: 0,
            invariant_id: null,
            consecutive_invariant_failures: 1,
          },
        }),
      }],
    }),
    /invariant_id is required when invariant failures are recorded/u,
  );
});

Deno.test("duplicate applications and invalid application counts are rejected", () => {
  assertInvalid(
    stateDocument({
      applications: [
        { app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null },
        { app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null },
      ],
    }),
    /applications contain duplicate apps/u,
  );
  const twoApps = stateDocument({
    applications: [
      { app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null },
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
      { app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null },
    ],
  });
  assertInvalid(twoApps, /applications must contain 1 to 2 entries/u);
  assertInvalid(stateDocument({ applications: [] }), /applications must contain 1 to 2 entries/u);
});

Deno.test("schema errors never include the supplied value or raw document", () => {
  const message = messageOf(() =>
    parseSentinelProviderStateDocument(stateDocument({
      applications: [{ app: "ai-ubq-fi", healthy: attestation({ git_sha: "ZZZZ_NOT_A_SHA" }), transaction: null }],
    }))
  );
  assert.match(message, /healthy\.git_sha/u);
  assert.doesNotMatch(message, /ZZZZ_NOT_A_SHA/u);
  const generationMessage = messageOf(() => parseSentinelProviderStateDocument(stateDocument({ generation: 1.5 })));
  assert.match(generationMessage, /provider_state\.generation/u);
  assert.doesNotMatch(generationMessage, /1\.5/u);
  assert.doesNotMatch(generationMessage, /schema_version|applications/u);
});

// ---------------------------------------------------------------------------
// Fake GitHub REST backend: content-addressed blobs/trees/commits with
// fast-forward ref semantics, sibling-writer hooks, and an optional read gate.
// ---------------------------------------------------------------------------

const sha1Hex = async (data: Uint8Array): Promise<string> => {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-1", Uint8Array.from(data)));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const STATE_REF = "refs/heads/sentinel/bootstrap-state";
const DEVELOPMENT_REF = "refs/heads/development";
const REPOSITORY = "ubiquity/ai.ubq.fi";

class FakeGitHubBackend {
  readonly requests: Array<{
    method: string;
    path: string;
    query: string;
    url: string;
    redirect: string;
  }> = [];
  readonly refUpdates: Array<Readonly<{ method: string; body: Record<string, unknown> }>> = [];
  readonly blobs = new Map<string, string>();
  readonly trees = new Map<string, ReadonlyMap<string, string>>();
  readonly commits = new Map<string, { tree: string; parents: readonly string[] }>();
  readonly refs = new Map<string, string>();
  /** Exact run-attempt GET responses, keyed by `/actions/runs/<id>/attempts/<n>`; factories return a fresh Response per request. */
  readonly attemptResponses = new Map<string, () => Response>();
  /** Runs before each attempt GET is served (drift hook). */
  beforeAttemptGet: (() => void | Promise<void>) | null = null;
  refReadGate: Promise<void> | null = null;
  beforeRefUpdate: (() => void | Promise<void>) | null = null;
  refResponseShaOverride: string | null = null;
  refUpdateErrorAfterProcessing: number | null = null;
  contentsEncodingOverride: string | null = null;

  private async putBlob(content: string): Promise<string> {
    const sha = await sha1Hex(new TextEncoder().encode(`blob:${content}`));
    this.blobs.set(sha, content);
    return sha;
  }

  private async putTree(
    base: string | null,
    entries: ReadonlyArray<Readonly<{ path: string; sha: string }>>,
  ): Promise<string> {
    const merged = new Map<string, string>(base === null ? [] : this.trees.get(base) ?? []);
    for (const entry of entries) merged.set(entry.path, entry.sha);
    const canonical = JSON.stringify(
      [...merged.entries()].sort(([left], [right]) => (left < right ? -1 : left > right ? 1 : 0)),
    );
    const sha = await sha1Hex(new TextEncoder().encode(`tree:${canonical}`));
    this.trees.set(sha, merged);
    return sha;
  }

  private async putCommit(message: string, tree: string, parents: readonly string[]): Promise<string> {
    const sha = await sha1Hex(
      new TextEncoder().encode(`commit:${message}:${tree}:${parents.join(",")}`),
    );
    this.commits.set(sha, { tree, parents });
    return sha;
  }

  private async siblingCommit(files: Readonly<Record<string, string>>, parent: string): Promise<string> {
    const entries = await Promise.all(
      Object.entries(files).map(async ([path, content]) => ({ path, sha: await this.putBlob(content) })),
    );
    const tree = await this.putTree(this.commits.get(parent)?.tree ?? null, entries);
    return await this.putCommit("chore(sentinel): sibling write", tree, [parent]);
  }

  async seedDevelopment(files: Readonly<Record<string, string>>): Promise<string> {
    const entries = await Promise.all(
      Object.entries(files).map(async ([path, content]) => ({ path, sha: await this.putBlob(content) })),
    );
    const tree = await this.putTree(null, entries);
    const commit = await this.putCommit("seed development", tree, []);
    this.refs.set(DEVELOPMENT_REF, commit);
    return commit;
  }

  stateRefHead(): string | null {
    return this.refs.get(STATE_REF) ?? null;
  }

  developmentHead(): string {
    return this.refs.get(DEVELOPMENT_REF)!;
  }

  async siblingWrite(files: Readonly<Record<string, string>>): Promise<string> {
    const parent = this.stateRefHead() ?? this.developmentHead();
    const commit = await this.siblingCommit(files, parent);
    this.refs.set(STATE_REF, commit);
    return commit;
  }

  async siblingCreateBranch(files: Readonly<Record<string, string>>): Promise<string> {
    const parent = this.developmentHead();
    const commit = await this.siblingCommit(files, parent);
    this.refs.set(STATE_REF, commit);
    return commit;
  }

  fileAtRef(path: string, commitSha: string): string | null {
    const tree = this.trees.get(this.commits.get(commitSha)?.tree ?? "");
    const blobSha = tree?.get(path);
    return blobSha === undefined ? null : this.blobs.get(blobSha) ?? null;
  }

  async headFile(path: string): Promise<string | null> {
    const head = this.stateRefHead();
    return head === null ? null : await this.fileAtRef(path, head);
  }

  childCommitsOf(parent: string): number {
    let count = 0;
    for (const commit of this.commits.values()) {
      if (commit.parents.includes(parent)) count += 1;
    }
    return count;
  }

  isAncestor(ancestor: string, descendant: string): boolean {
    const seen = new Set<string>();
    const queue = [descendant];
    while (queue.length > 0) {
      const current = queue.shift()!;
      if (current === ancestor) return true;
      if (seen.has(current)) continue;
      seen.add(current);
      const commit = this.commits.get(current);
      if (commit !== undefined) queue.push(...commit.parents);
    }
    return false;
  }

  private refResponse(name: string): Promise<Response> {
    const sha = this.refs.get(name);
    return Promise.resolve(
      sha === undefined
        ? new Response("not found", { status: 404 })
        : Response.json({ ref: name, object: { sha, type: "commit" } }),
    );
  }

  private async gated<T>(run: () => Promise<T>): Promise<T> {
    if (this.refReadGate !== null) await this.refReadGate;
    return await run();
  }

  readonly fetcher: typeof fetch = async (input, init = {}) => {
    const url = new URL(String(input));
    const path = url.pathname.replace(/^\/repos\/[^/]+\/[^/]+/u, "");
    const query = url.search;
    this.requests.push({
      method: init.method ?? "GET",
      path,
      query,
      url: String(input),
      redirect: init.redirect ?? "follow",
    });
    if (init.method === "PATCH" || init.method === "POST") {
      if (path === "/git/refs" || path.endsWith("/sentinel/bootstrap-state")) {
        this.refUpdates.push({
          method: init.method,
          body: JSON.parse(String(init.body ?? "{}")) as Record<string, unknown>,
        });
      }
    }
    if (path === "/git/ref/heads/sentinel/bootstrap-state") {
      return this.gated(() => this.refResponse(STATE_REF));
    }
    if (path === "/git/ref/heads/development") {
      return this.refResponse(DEVELOPMENT_REF);
    }
    if (path === "/git/refs/heads/sentinel/bootstrap-state") {
      return this.refUpdate();
    }
    if (path === "/git/refs") {
      return this.refCreate();
    }
    const attemptMatch = path.match(/^\/actions\/runs\/([0-9]+)\/attempts\/([0-9]+)$/u);
    if (attemptMatch) {
      if (this.beforeAttemptGet !== null) await this.beforeAttemptGet();
      const factory = this.attemptResponses.get(path);
      if (factory === undefined) {
        throw new Error(`Unexpected fake GitHub attempt request ${init.method ?? "GET"} ${path}`);
      }
      return await factory();
    }
    const commitMatch = path.match(/^\/git\/commits\/([0-9a-f]{40})$/u);
    if (commitMatch) {
      const commit = this.commits.get(commitMatch[1]!);
      return Promise.resolve(
        commit === undefined
          ? new Response("unknown commit", { status: 404 })
          : Response.json({ sha: commitMatch[1], tree: { sha: commit.tree } }),
      );
    }
    const contentsMatch = path.match(/^\/contents\/(.+)$/u);
    if (contentsMatch) {
      const ref = url.searchParams.get("ref") ?? "";
      const commit = url.pathname === "" ? undefined : this.commits.get(ref);
      const tree = commit === undefined ? undefined : this.trees.get(commit.tree);
      const blobSha = tree?.get(contentsMatch[1]!);
      if (commit === undefined || blobSha === undefined) {
        return Promise.resolve(new Response("not found", { status: 404 }));
      }
      const content = this.blobs.get(blobSha)!;
      return Promise.resolve(Response.json({
        sha: blobSha,
        type: "file",
        encoding: this.contentsEncodingOverride ?? "base64",
        content: btoa(content),
      }));
    }
    if (path === "/git/blobs") {
      const body = JSON.parse(String(init.body ?? "{}")) as { content?: unknown; encoding?: unknown };
      if (typeof body.content !== "string") return Promise.resolve(new Response("bad blob", { status: 422 }));
      const content = body.encoding === "base64"
        ? new TextDecoder().decode(Uint8Array.from(atob(body.content), (char) => char.charCodeAt(0)))
        : body.content;
      return Promise.resolve(Response.json({ sha: await this.putBlob(content) } as unknown as JsonResponse));
    }
    if (path === "/git/trees") {
      const body = JSON.parse(String(init.body ?? "{}")) as {
        base_tree?: string | null;
        tree?: Array<{ path?: unknown; sha?: unknown }>;
      };
      const entries = (body.tree ?? []).map((entry) => ({
        path: String(entry.path),
        sha: String(entry.sha),
      }));
      return Promise.resolve(Response.json({ sha: await this.putTree(body.base_tree ?? null, entries) } as unknown));
    }
    if (path === "/git/commits") {
      const body = JSON.parse(String(init.body ?? "{}")) as {
        message?: unknown;
        tree?: unknown;
        parents?: readonly unknown[];
      };
      const tree = String(body.tree ?? "");
      const parents = (body.parents ?? []).map(String);
      const commit = await this.putCommit(String(body.message ?? ""), tree, parents);
      return Promise.resolve(Response.json({ sha: commit, tree: { sha: tree } }));
    }
    throw new Error(`Unexpected fake GitHub request ${init.method ?? "GET"} ${path}`);
  };

  private async refCreate(): Promise<Response> {
    const hook = this.beforeRefUpdate;
    this.beforeRefUpdate = null;
    if (hook !== null) await hook();
    const body = (this.refUpdates.at(-1)?.body ?? {}) as { ref?: unknown; sha?: unknown };
    const name = String(body.ref ?? "");
    const sha = String(body.sha ?? "");
    if (this.refs.has(name) || !this.commits.has(sha)) {
      return Promise.resolve(Response.json({ message: "reference already exists" }, { status: 422 }));
    }
    this.refs.set(name, sha);
    return Promise.resolve(Response.json({ ref: name, object: { sha, type: "commit" } }));
  }

  private async refUpdate(): Promise<Response> {
    const hook = this.beforeRefUpdate;
    this.beforeRefUpdate = null;
    if (hook !== null) await hook();
    const body = (this.refUpdates.at(-1)?.body ?? {}) as { sha?: unknown; force?: unknown };
    const name = STATE_REF;
    const incoming = String(body.sha ?? "");
    const current = this.refs.get(name);
    if (current === undefined || !this.commits.has(incoming) || !this.isAncestor(current, incoming)) {
      return Promise.resolve(Response.json({ message: "update is not a fast-forward" }, { status: 422 }));
    }
    this.refs.set(name, incoming);
    const responseSha = this.refResponseShaOverride ?? incoming;
    if (this.refUpdateErrorAfterProcessing !== null) {
      return Promise.resolve(new Response("lost response", { status: this.refUpdateErrorAfterProcessing }));
    }
    return Promise.resolve(Response.json({ ref: name, object: { sha: responseSha, type: "commit" } }));
  }
}

type JsonResponse = { sha: string };

const renderBootstrapDocument = (): string =>
  JSON.stringify(
    parseSentinelBootstrapStateDocument({
      schema_version: 1,
      release: null,
      signals: [],
      activation: null,
      rollback_intent: null,
      constraints: [],
      progress: null,
    }),
    null,
    2,
  ) + "\n";

const bootstrapRelease = (): SentinelBootstrapReleaseRecordV1 => ({
  schema_version: 1,
  stable_sha: SHA_A,
  candidate_sha: SHA_B,
  acceptance_evidence: ["ci:stable"],
  activated_at: CREATED_AT,
  rollback_reason: null,
  generation: 1,
});

const providerStateJson = (document: unknown): string => `${JSON.stringify(parseOk(document), null, 2)}\n`;

const makeAdapter = (backend: FakeGitHubBackend) =>
  createGitHubSentinelProviderState({ token: "test-token", repository: REPOSITORY, fetcher: backend.fetcher });

const countRequestsWhere = (backend: FakeGitHubBackend, method: string): number =>
  backend.requests.filter((request) => request.method === method).length;

Deno.test("a missing provider file and absent branch read as explicit uninitialized state with zero writes", async () => {
  const backend = new FakeGitHubBackend();
  const developmentSha = await backend.seedDevelopment({ "docs/other.txt": "other" });
  const adapter = await makeAdapter(backend);
  const snapshot = adapter.readSnapshot();
  assert.equal(snapshot.document, null, "a missing provider file returns explicit uninitialized state");
  assert.equal(snapshot.commit_sha, developmentSha);
  assert.equal(snapshot.state_ref_exists, false);
  assert.equal(backend.stateRefHead(), null);
  assert.equal(backend.blobs.size, 1); // only the seeded content blob exists
  assert.equal(countRequestsWhere(backend, "POST"), 0, "reads must never write");
  assert.ok(
    backend.requests.every((request) => request.path.startsWith("/git/") || request.path.startsWith("/contents/")),
    "no health or deployment API is ever requested",
  );
});

Deno.test("corrupt controller JSON is independent from provider-state reads", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({
    [SENTINEL_BOOTSTRAP_STATE_PATH]: "{ not valid controller json",
    [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)),
  });
  const provider = await makeAdapter(backend);
  const snapshot = provider.readSnapshot();
  assert.equal(snapshot.document!.generation, 1);
  assert.ok(
    !backend.requests.some((request) => request.path.includes(SENTINEL_BOOTSTRAP_STATE_PATH)),
    "the provider adapter must never read the controller document",
  );
  await assert.rejects(
    createGitHubSentinelBootstrapState({ token: "test-token", repository: REPOSITORY, fetcher: backend.fetcher }),
    SyntaxError,
    "corrupt controller JSON must fail the controller reader independently",
  );
});

Deno.test("malformed provider contents, JSON, and schema always throw", async () => {
  const jsonBackend = new FakeGitHubBackend();
  await jsonBackend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: "{" });
  await assert.rejects(makeAdapter(jsonBackend), /not valid JSON/u);
  const schemaBackend = new FakeGitHubBackend();
  await schemaBackend.seedDevelopment({
    [SENTINEL_PROVIDER_STATE_PATH]: JSON.stringify({ schema_version: 2 }),
  });
  await assert.rejects(makeAdapter(schemaBackend), /schema_version must be 1/u);
  const encodingBackend = new FakeGitHubBackend();
  encodingBackend.contentsEncodingOverride = "none";
  await encodingBackend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: "{}" });
  await assert.rejects(makeAdapter(encodingBackend), /file is invalid/u);
});

Deno.test("initial CAS publishes generation 1 on the missing branch and preserves the tree contents", async () => {
  const backend = new FakeGitHubBackend();
  const developmentSha = await backend.seedDevelopment({
    [SENTINEL_BOOTSTRAP_STATE_PATH]: renderBootstrapDocument(),
    "docs/other.txt": "other contents",
  });
  const adapter = await makeAdapter(backend);
  const expected = developmentSha;
  const next = parseOk(stateDocument({
    generation: 1,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null }],
  }));
  assert.equal(await adapter.compareAndSet(expected, next), true);
  const head = backend.stateRefHead()!;
  assert.notEqual(head, developmentSha);
  const headCommit = backend.commits.get(head)!;
  assert.deepEqual(headCommit.parents, [developmentSha], "exactly the captured parent must be used");
  assert.equal(
    await backend.headFile(SENTINEL_PROVIDER_STATE_PATH),
    providerStateJson(next),
    "the provider document must be byte-deterministic",
  );
  assert.equal(
    await backend.headFile(SENTINEL_BOOTSTRAP_STATE_PATH),
    renderBootstrapDocument(),
    "controller file must be preserved",
  );
  assert.equal(await backend.headFile("docs/other.txt"), "other contents");
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 1);
  assert.equal(refreshed.commit_sha, head);
  assert.equal(refreshed.state_ref_exists, true);
  const contentsRead = backend.requests.filter((request) => request.path.includes(SENTINEL_PROVIDER_STATE_PATH)).at(
    -1,
  )!;
  assert.equal(
    new URLSearchParams(contentsRead.query).get("ref"),
    refreshed.commit_sha,
    "reads are pinned to the exact immutable commit",
  );
  assert.ok(Object.isFrozen(refreshed.document));
});

Deno.test("stale local expected SHA rejects with false and no request", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)) });
  const adapter = await makeAdapter(backend);
  const before = backend.requests.length;
  assert.equal(
    await adapter.compareAndSet("f".repeat(40), parseOk(stateDocument({ generation: 2, applications: [appState()] }))),
    false,
  );
  assert.equal(backend.requests.length, before, "stale expected SHA must not issue a request");
});

Deno.test("initial generation must be 1 and may never carry a transaction", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(backend.developmentHead(), parseOk(stateDocument({ generation: 2 }))),
    false,
    "a missing document must initialize at generation 1",
  );
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({
        generation: 1,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: preparedTransaction({
            fence_generation: 1,
            previous_transaction_commit: null,
          }),
        }],
      })),
    ),
    false,
    "a new application must never enter with a transaction",
  );
  assert.equal(backend.stateRefHead(), null);
});

Deno.test("generation must advance by exactly one and application entries are retained", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  const first = parseOk(stateDocument({
    generation: 1,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null }],
  }));
  assert.equal(await adapter.compareAndSet(backend.developmentHead(), first), true);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(head, parseOk(stateDocument({ generation: 1, applications: [appState()] }))),
    false,
    "generation must advance by exactly one",
  );
  assert.equal(
    await adapter.compareAndSet(head, parseOk(stateDocument({ generation: 3, applications: [appState()] }))),
    false,
    "a generation jump is rejected",
  );
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 2,
        applications: [
          { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
        ],
      })),
    ),
    false,
    "existing application entries must be retained",
  );
});

Deno.test("nonterminal transactions cannot be removed, replaced, or have protected fields changed", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
  );
  let head = adapter.readSnapshot().commit_sha;
  const prepared = preparedTransaction();
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: prepared,
        }],
      })),
    ),
    true,
    "a first transaction may start from healthy plus null at the next generation",
  );
  head = adapter.readSnapshot().commit_sha;
  const removeAttempt = stateDocument({
    generation: 3,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null }],
  });
  assert.equal(
    await adapter.compareAndSet(head, parseOk(removeAttempt)),
    false,
    "a nonterminal transaction cannot be removed",
  );
  const idAttempt = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({ id: "tx-2" }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, idAttempt), false, "the transaction id is pinned");
  const changedPrevious = attestation({ verified_at: VERIFIED_B });
  const previousAttempt = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: changedPrevious,
      transaction: preparedTransaction({ previous: changedPrevious }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, previousAttempt), false, "the previous attestation is pinned");
  const candidateAttempt = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({
        candidate: attestation({ git_sha: SHA_C, revision_id: "rev-c", verified_at: VERIFIED_B }),
        expected_merged_sha: SHA_C,
      }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, candidateAttempt), false, "the candidate attestation is pinned");
  const executorAttempt = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({ executor: executor({ run_id: 43 }) }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, executorAttempt), false, "executor replacement is prohibited");
  const fenceAttempt = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({ fence_generation: 3 }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, fenceAttempt), false, "fence replacement is prohibited");
  assert.equal(backend.childCommitsOf(head), 0, "rejected CAS attempts must publish nothing");
});

Deno.test("a new transaction after a terminal one requires the exact captured commit and prepared phase", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
  );
  let head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 2,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: preparedTransaction(),
        }],
      })),
    ),
    true,
  );
  head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 3,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: promotionPending(),
        }],
      })),
    ),
    true,
  );
  head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 4,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: observing(),
        }],
      })),
    ),
    true,
  );
  head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 5,
        applications: [{
          app: "ai-ubq-fi",
          healthy: candidateAttestation(),
          transaction: keptTransaction(),
        }],
      })),
    ),
    true,
  );
  head = adapter.readSnapshot().commit_sha;
  // While the terminal transaction is current, only a prepared transaction
  // anchored at the exact captured commit may follow.
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 6,
        applications: [{
          app: "ai-ubq-fi",
          healthy: candidateAttestation(),
          transaction: preparedTransaction({
            id: "tx-3",
            fence_generation: 6,
            previous: candidateAttestation(),
            candidate: attestation({ git_sha: SHA_C, revision_id: "rev-c", verified_at: VERIFIED_B }),
            expected_merged_sha: SHA_C,
            previous_transaction_commit: null,
          }),
        }],
      })),
    ),
    false,
    "previous_transaction_commit must equal the captured commit",
  );
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 6,
        applications: [{
          app: "ai-ubq-fi",
          healthy: candidateAttestation(),
          transaction: preparedTransaction({
            id: "tx-4",
            fence_generation: 4,
            previous: candidateAttestation(),
            candidate: attestation({ git_sha: SHA_C, revision_id: "rev-c", verified_at: VERIFIED_B }),
            expected_merged_sha: SHA_C,
            previous_transaction_commit: head,
          }),
        }],
      })),
    ),
    false,
    "fence_generation must equal the next document generation",
  );
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 6,
        applications: [{
          app: "ai-ubq-fi",
          healthy: candidateAttestation(),
          transaction: promotionPending({
            id: "tx-5",
            fence_generation: 6,
            previous: candidateAttestation(),
            candidate: attestation({ git_sha: SHA_C, revision_id: "rev-c", verified_at: VERIFIED_B }),
            expected_merged_sha: SHA_C,
            previous_transaction_commit: head,
          }),
        }],
      })),
    ),
    false,
    "a new transaction after a terminal one must be prepared",
  );
  const nextTransaction = preparedTransaction({
    id: "tx-2",
    fence_generation: 6,
    previous: candidateAttestation(),
    candidate: attestation({ git_sha: SHA_C, revision_id: "rev-c", verified_at: VERIFIED_B }),
    expected_merged_sha: SHA_C,
    previous_transaction_commit: head,
    created_at: "2026-09-04T12:50:00.000Z",
  });
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 6,
        applications: [{ app: "ai-ubq-fi", healthy: candidateAttestation(), transaction: nextTransaction }],
      })),
    ),
    true,
    "a prepared transaction may start at the exact captured commit after a terminal one",
  );
});

Deno.test("a new application may not enter with a transaction but may join without one", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
  );
  const head = adapter.readSnapshot().commit_sha;
  const withTransaction = parseOk(stateDocument({
    generation: 2,
    applications: [
      appState(),
      {
        app: "p-ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({
          id: "tx-p",
          fence_generation: 2,
        }),
      },
    ],
  }));
  assert.equal(await adapter.compareAndSet(head, withTransaction), false);
  const withoutTransaction = parseOk(stateDocument({
    generation: 2,
    applications: [
      appState(),
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
    ],
  }));
  assert.equal(await adapter.compareAndSet(head, withoutTransaction), true);
});

Deno.test("every application's transaction and retention are validated before any publish", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  const twoAppsInitial = parseOk(stateDocument({
    generation: 1,
    applications: [
      { app: "ai-ubq-fi", healthy: previousAttestation(), transaction: null },
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
    ],
  }));
  assert.equal(await adapter.compareAndSet(backend.developmentHead(), twoAppsInitial), true);
  let head = adapter.readSnapshot().commit_sha;
  // A valid first-app transaction must never let the second app be deleted.
  const deleteSecondApp = parseOk(stateDocument({
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({
        id: "tx-a",
        fence_generation: 2,
      }),
    }],
  }));
  assert.equal(
    await adapter.compareAndSet(head, deleteSecondApp),
    false,
    "a valid first-app transaction must never permit deleting the second app",
  );
  assert.equal(backend.childCommitsOf(head), 0);
  const bothActive = parseOk(stateDocument({
    applications: [
      {
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({ id: "tx-a", fence_generation: 2 }),
      },
      {
        app: "p-ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({ id: "tx-b", fence_generation: 2 }),
      },
    ],
  }));
  assert.equal(await adapter.compareAndSet(head, bothActive), true);
  head = adapter.readSnapshot().commit_sha;
  const changedSecondIdentity = parseOk(stateDocument({
    generation: 3,
    applications: [
      {
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({ id: "tx-a", fence_generation: 2 }),
      },
      {
        app: "p-ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: preparedTransaction({ id: "tx-b2", fence_generation: 3 }),
      },
    ],
  }));
  assert.equal(
    await adapter.compareAndSet(head, changedSecondIdentity),
    false,
    "the second app's active transaction identity may never change",
  );
  assert.equal(backend.childCommitsOf(head), 0);
});

Deno.test("terminal transactions persist only exactly equal with an unchanged healthy attestation", async () => {
  const backend = new FakeGitHubBackend();
  // A kept snapshot is only reachable through the full release path, which
  // this test does not exercise: seed an already valid kept document on the
  // state branch through the sibling-writer facade so the terminal
  // equality/clear/regression assertions below see it as current at
  // generation 3, exactly as the original prepared->kept setup did.
  const kept = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: candidateAttestation(),
      transaction: keptTransaction(),
    }],
  }));
  await backend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(kept) });
  await backend.siblingCreateBranch({});
  const adapter = await makeAdapter(backend);
  let head = adapter.readSnapshot().commit_sha;
  assert.equal(
    adapter.readSnapshot().document!.applications[0]!.transaction!.phase,
    "kept",
    "the seeded state branch must present the kept transaction as current",
  );
  const unchanged = parseOk(stateDocument({
    generation: 4,
    applications: [{
      app: "ai-ubq-fi",
      healthy: candidateAttestation(),
      transaction: keptTransaction(),
    }],
  }));
  assert.equal(
    await adapter.compareAndSet(head, unchanged),
    true,
    "an exactly equal terminal transaction persists with its healthy attestation",
  );
  head = adapter.readSnapshot().commit_sha;
  const postsAfterPersist = countRequestsWhere(backend, "POST");
  const patchesAfterPersist = countRequestsWhere(backend, "PATCH");
  const clear = parseOk(stateDocument({
    generation: 5,
    applications: [{
      app: "ai-ubq-fi",
      healthy: candidateAttestation(),
      transaction: null,
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, clear), false, "a terminal transaction can never be cleared");
  const regress = parseOk(stateDocument({
    generation: 5,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: promotionPending(),
    }],
  }));
  assert.equal(
    await adapter.compareAndSet(head, regress),
    false,
    "terminal evidence can never regress to a nonterminal phase",
  );
  const rewrite = parseOk(stateDocument({
    generation: 5,
    applications: [{
      app: "ai-ubq-fi",
      healthy: candidateAttestation(),
      transaction: keptTransaction({ reason: "rewritten_evidence" }),
    }],
  }));
  assert.equal(await adapter.compareAndSet(head, rewrite), false, "completed evidence can never be rewritten");
  assert.equal(backend.childCommitsOf(head), 0, "rejected terminal persistence must publish nothing");
  assert.equal(
    countRequestsWhere(backend, "POST"),
    postsAfterPersist,
    "rejected terminal persistence must create no additional Git objects",
  );
  assert.equal(
    countRequestsWhere(backend, "PATCH"),
    patchesAfterPersist,
    "rejected terminal persistence must never update the state ref",
  );
});

Deno.test("a remote sibling writer between snapshot and CAS is rejected before any publish", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)) });
  const adapter = await makeAdapter(backend);
  await backend.siblingWrite({
    [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({ generation: 2, applications: [appState()] })),
  });
  const headSha = await backend.siblingWrite({ "docs/other.txt": "sibling" });
  assert.equal(
    await adapter.compareAndSet(
      adapter.readSnapshot().commit_sha,
      parseOk(stateDocument({ generation: 2, applications: [appState()] })),
    ),
    false,
    "the changed remote ref must reject the stale writer",
  );
  assert.equal(countRequestsWhere(backend, "POST"), 0, "nothing may be published after the pre-check");
  assert.equal(backend.stateRefHead(), headSha);
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.commit_sha, headSha);
});

Deno.test("a racing sibling ref update is a conflict that refreshes without blind retry", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({
    [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)),
  });
  await backend.siblingCreateBranch({});
  const adapter = await makeAdapter(backend);
  backend.beforeRefUpdate = async () => {
    await backend.siblingWrite({ "docs/from-sibling.txt": "sibling" });
  };
  assert.equal(
    await adapter.compareAndSet(
      adapter.readSnapshot().commit_sha,
      parseOk(stateDocument({ generation: 2, applications: [appState()] })),
    ),
    false,
    "409/422 ref publication is a conflict, not success",
  );
  assert.equal(backend.refUpdates.filter((update) => update.method === "PATCH").length, 1, "no blind retry");
  const refreshed = await adapter.refresh();
  assert.equal(
    await backend.headFile("docs/from-sibling.txt"),
    "sibling",
    "the refresh must observe the sibling's winning state",
  );
  assert.equal(refreshed.document!.generation, 1, "the sibling kept generation 1");
});

Deno.test("a sibling branch creation races the initial ref POST and resolves as conflict plus refresh", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({ "docs/other.txt": "other" });
  const adapter = await makeAdapter(backend);
  backend.beforeRefUpdate = async () => {
    await backend.siblingCreateBranch({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)) });
  };
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    false,
  );
  assert.equal(backend.refUpdates.filter((update) => update.method === "POST").length, 1);
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 1);
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
});

Deno.test("provider and controller updates share one branch without clobbering each other", async () => {
  const backend = new FakeGitHubBackend();
  const developmentSha = await backend.seedDevelopment({
    [SENTINEL_BOOTSTRAP_STATE_PATH]: renderBootstrapDocument(),
    "docs/other.txt": "other",
  });
  const provider = await makeAdapter(backend);
  const controller = await createGitHubSentinelBootstrapState({
    token: "test-token",
    repository: REPOSITORY,
    fetcher: backend.fetcher,
  });
  // Controller writes first; the provider's stale snapshot must refuse.
  await controller.replaceRelease(bootstrapRelease());
  assert.equal(
    await provider.compareAndSet(developmentSha, parseOk(stateDocument({ generation: 1, applications: [appState()] }))),
    false,
  );
  const afterController = await provider.refresh();
  assert.equal(afterController.document, null, "provider file is still absent at the shared head");
  assert.equal(
    await provider.compareAndSet(
      afterController.commit_sha,
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
    "the provider may publish into the controller-created branch",
  );
  assert.equal(
    await backend.headFile(SENTINEL_BOOTSTRAP_STATE_PATH),
    JSON.stringify(controller.readDocument(), null, 2) + "\n",
  );
  assert.equal(await backend.headFile("docs/other.txt"), "other");
  // A fresh controller store reads the shared head and preserves the provider file.
  const controller2 = await createGitHubSentinelBootstrapState({
    token: "test-token",
    repository: REPOSITORY,
    fetcher: backend.fetcher,
  });
  await controller2.replaceRelease(bootstrapRelease());
  const controllerDocument = await backend.headFile(SENTINEL_BOOTSTRAP_STATE_PATH);
  assert.ok(controllerDocument);
  assert.equal(parseSentinelBootstrapStateDocument(JSON.parse(controllerDocument)).release?.generation, 1);
  assert.equal(
    await backend.headFile(SENTINEL_PROVIDER_STATE_PATH),
    providerStateJson(stateDocument({ generation: 1, applications: [appState()] })),
  );
  // The provider advances on the same branch, preserving the controller file again.
  const provider2 = await makeAdapter(backend);
  assert.equal(
    await provider2.compareAndSet(
      provider2.readSnapshot().commit_sha,
      parseOk(stateDocument({ generation: 2, applications: [appState()] })),
    ),
    true,
  );
  assert.equal(await backend.headFile(SENTINEL_BOOTSTRAP_STATE_PATH), controllerDocument);
});

Deno.test("a lost publication response throws instead of claiming success and refresh reconciles without replay", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  await backend.siblingCreateBranch({});
  const initialSha = backend.stateRefHead()!;
  const adapter = await makeAdapter(backend);
  backend.refUpdateErrorAfterProcessing = 500;
  await assert.rejects(
    adapter.compareAndSet(initialSha, parseOk(stateDocument({ generation: 1, applications: [appState()] }))),
    /HTTP 500/u,
  );
  assert.equal(
    backend.refUpdates.filter((update) => update.method === "PATCH").length,
    1,
    "no replay after a failed response",
  );
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 1, "the applied update must be observable through an exact refresh");
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
});

Deno.test("an ambiguous successful ref response is an explicit error that refresh can reconcile", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  await backend.siblingCreateBranch({});
  const initialSha = backend.stateRefHead()!;
  const adapter = await makeAdapter(backend);
  backend.refResponseShaOverride = "f".repeat(40);
  await assert.rejects(
    adapter.compareAndSet(initialSha, parseOk(stateDocument({ generation: 1, applications: [appState()] }))),
    /ambiguous/u,
  );
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 1);
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
  assert.equal(
    backend.refUpdates.filter((update) => update.method === "PATCH").length,
    1,
    "no blind retry after an ambiguous response",
  );
});

Deno.test("concurrent compareAndSet calls on one adapter cannot publish a duplicate generation", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
  );
  const captured = adapter.readSnapshot().commit_sha;
  let releaseGate!: () => void;
  backend.refReadGate = new Promise<void>((resolve) => {
    releaseGate = resolve;
  });
  const first = parseOk(stateDocument({
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({ id: "tx-a" }),
    }],
  }));
  const second = first;
  const outcomes = await (async () => {
    const firstCall = adapter.compareAndSet(captured, first);
    const secondCall = adapter.compareAndSet(captured, second);
    releaseGate();
    return await Promise.all([firstCall, secondCall]);
  })();
  assert.equal(outcomes.filter(Boolean).length, 1, "exactly one concurrent CAS may win");
  assert.equal(backend.stateRefHead() !== null, true);
  const finalDocument = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  assert.equal(finalDocument.generation, 2, "a duplicate generation must never be published");
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 2);
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
});

Deno.test("readSnapshot documents are immutable and refresh never reuses cached content", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(stateDocument({}, 1)) });
  const adapter = await makeAdapter(backend);
  const first = adapter.readSnapshot();
  assert.ok(Object.isFrozen(first.document));
  assert.throws(() => {
    (first as { commit_sha: string }).commit_sha = "x";
  }, TypeError);
  const sibling = providerStateJson(stateDocument({
    generation: 2,
    applications: [appState()],
  }));
  await backend.siblingWrite({ [SENTINEL_PROVIDER_STATE_PATH]: sibling });
  const refreshed = await adapter.refresh();
  assert.equal(
    await backend.headFile(SENTINEL_PROVIDER_STATE_PATH),
    sibling,
    "refresh must observe the exact remote state",
  );
  assert.equal(refreshed.document!.generation, 2);
  assert.notEqual(refreshed.commit_sha, first.commit_sha);
});

Deno.test("cross-field evidence skews are rejected even when each field stays schema-valid", () => {
  // An observation carries an invariant identity while samples is zero.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: previousAttestation(),
        transaction: observing({
          observation: {
            ...zeroObservation(),
            invariant_id: "fixed-invariant",
          },
        }),
      }],
    }),
    /must be empty when samples is 0/u,
  );
  // Reject a kept route before the kept observation.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: candidateAttestation(),
        transaction: keptTransaction({
          route: {
            revision_id: "rev-b",
            observed_at: "2026-09-04T12:39:59.999Z",
            evidence_ref: "github:stale-route",
          },
        }),
      }],
    }),
    /route.observed_at cannot precede the kept observation/u,
  );
  // Reject a promotion result after the kept observation.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: candidateAttestation(),
        transaction: keptTransaction({
          promotion_result: {
            ...acknowledged(),
            observed_at: "2026-09-04T12:40:00.001Z",
          },
        }),
      }],
    }),
    /promotion_result.observed_at cannot follow the kept observation/u,
  );
  // Reject a restored route before the rollback result.
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: restorationAttestation(),
        transaction: rolledBackTransaction({
          route: {
            revision_id: "rev-a",
            observed_at: "2026-09-04T13:04:59.999Z",
            evidence_ref: "github:stale-restored-route",
          },
        }),
      }],
    }),
    /route.observed_at cannot precede the rollback result/u,
  );
  // Reject restoration evidence before the rollback result.
  const restoration = attestation({ verified_at: "2026-09-04T13:04:59.999Z" });
  assertInvalid(
    stateDocument({
      applications: [{
        app: "ai-ubq-fi",
        healthy: restoration,
        transaction: rolledBackTransaction({ restoration }),
      }],
    }),
    /restoration.verified_at cannot precede the rollback result/u,
  );
});

Deno.test("healthy attestations initialize once, are retained, can never be cleared, and cannot be replaced outside a release transaction", async () => {
  // Replacement is prohibited outside a release transaction; successful kept
  // or rolled-back transactions may update healthy.
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState({ healthy: null })] })),
    ),
    true,
    "an empty app may initialize with healthy unset",
  );
  assert.equal(
    await adapter.compareAndSet(
      adapter.readSnapshot().commit_sha,
      parseOk(stateDocument({ generation: 2, applications: [appState()] })),
    ),
    true,
    "the next generation may explicitly initialize healthy",
  );
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({ generation: 3, applications: [appState({ healthy: null })] })),
    ),
    false,
    "an initialized healthy attestation can never be cleared",
  );
  assert.equal(backend.childCommitsOf(head), 0, "a rejected healthy clear must publish nothing");
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({ generation: 3, applications: [appState({ healthy: candidateAttestation() })] })),
    ),
    false,
    "an initialized healthy attestation can never be replaced outside a release transaction",
  );
  assert.equal(backend.childCommitsOf(head), 0, "a rejected healthy replacement must publish nothing");
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({ generation: 3, applications: [appState()] })),
    ),
    true,
    "an unchanged healthy attestation advances normally",
  );
});

Deno.test("transaction origin and retired executor evidence are pinned once the transaction exists", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  assert.equal(
    await adapter.compareAndSet(
      backend.developmentHead(),
      parseOk(stateDocument({ generation: 1, applications: [appState()] })),
    ),
    true,
  );
  assert.equal(
    await adapter.compareAndSet(
      adapter.readSnapshot().commit_sha,
      parseOk(stateDocument({ generation: 2, applications: [appState({ transaction: preparedTransaction() })] })),
    ),
    true,
  );
  const head = adapter.readSnapshot().commit_sha;
  const rewoundOrigin = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({ created_at: "2026-09-04T11:01:00.000Z" }),
    }],
  }));
  assert.equal(
    await adapter.compareAndSet(head, rewoundOrigin),
    false,
    "created_at is part of the pinned transaction identity",
  );
  const retiredEvidence = parseOk(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: preparedTransaction({
        retired_executor: {
          executor: { ...executor(), run_id: 9999 },
          conclusion: "cancelled",
          observed_at: "2026-09-04T11:02:00.000Z",
          evidence_ref: "github:retired-run",
        },
      }),
    }],
  }));
  assert.equal(
    await adapter.compareAndSet(head, retiredEvidence),
    false,
    "retired executor evidence cannot be attached to an existing transaction",
  );
  assert.equal(backend.stateRefHead(), head, "rejected attempts must not move the state ref");
  assert.equal(backend.childCommitsOf(head), 0, "rejected attempts must publish nothing");
});

// ---------------------------------------------------------------------------
// Same-ID release progress/evidence coverage.
// ---------------------------------------------------------------------------

// An observing transaction with recorded promotion, route, and observation
// evidence used as the seeded baseline for the negative and positive
// same-ID progress cases below.
const seededObserving = (overrides: Record<string, unknown> = {}): Fixture =>
  observing({
    route: { revision_id: "rev-b", observed_at: OBSERVED, evidence_ref: "github:route-candidate" },
    observation: {
      last_observed_at: "2026-09-04T12:35:00.000Z",
      samples: 2,
      consecutive_liveness_failures: 1,
      consecutive_inference_failures: 0,
      invariant_id: "fix-1",
      consecutive_invariant_failures: 1,
    },
    ...overrides,
  });

// Seeds a schema-valid provider document on development and publishes the
// state branch via the sibling-writer facade, so the adapter's captured
// snapshot presents the document as already current.
const seedStateBranch = async (document: Fixture): Promise<FakeGitHubBackend> => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({ [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(parseOk(document)) });
  await backend.siblingCreateBranch({});
  return backend;
};

Deno.test("an acknowledged observing snapshot rejects backward phases and every evidence rewrite before any publish", async () => {
  const backend = await seedStateBranch(stateDocument({
    generation: 2,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: seededObserving() }],
  }));
  const adapter = await makeAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  const attempts: ReadonlyArray<readonly [string, Fixture]> = [
    ["backward to prepared", preparedTransaction()],
    ["backward to promotion_pending clears promotion evidence", promotionPending()],
    [
      "acknowledged promotion result rewritten",
      seededObserving({ promotion_result: { ...acknowledged(), evidence_ref: "github:promotion-rewritten" } }),
    ],
    [
      "promotion result time rewound",
      seededObserving({ promotion_result: { ...acknowledged(), observed_at: "2026-09-04T12:09:00.000Z" } }),
    ],
    ["route cleared", seededObserving({ route: null })],
    [
      "route rewound to an older observation",
      seededObserving({
        route: { revision_id: "rev-b", observed_at: "2026-09-04T12:20:00.000Z", evidence_ref: "github:older-route" },
      }),
    ],
    [
      "route rewritten to the previous revision at an equal timestamp",
      seededObserving({
        route: { revision_id: "rev-a", observed_at: OBSERVED, evidence_ref: "github:route-previous" },
      }),
    ],
    [
      "observation last-seen time rewound at equal samples",
      seededObserving({
        observation: {
          last_observed_at: "2026-09-04T12:34:00.000Z",
          samples: 2,
          consecutive_liveness_failures: 1,
          consecutive_inference_failures: 0,
          invariant_id: "fix-1",
          consecutive_invariant_failures: 1,
        },
      }),
    ],
    [
      "observation samples decreased",
      seededObserving({
        observation: {
          last_observed_at: "2026-09-04T12:35:00.000Z",
          samples: 1,
          consecutive_liveness_failures: 1,
          consecutive_inference_failures: 0,
          invariant_id: "fix-1",
          consecutive_invariant_failures: 1,
        },
      }),
    ],
    [
      "same-sample consecutive counters changed",
      seededObserving({
        observation: {
          last_observed_at: "2026-09-04T12:35:00.000Z",
          samples: 2,
          consecutive_liveness_failures: 2,
          consecutive_inference_failures: 0,
          invariant_id: "fix-1",
          consecutive_invariant_failures: 1,
        },
      }),
    ],
  ];
  for (const [label, transaction] of attempts) {
    assert.equal(
      await adapter.compareAndSet(
        head,
        parseOk(stateDocument({
          generation: 3,
          applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction }],
        })),
      ),
      false,
      label,
    );
  }
  assert.equal(countRequestsWhere(backend, "POST"), 0, "rejected attempts must create no Git objects");
  assert.equal(countRequestsWhere(backend, "PATCH"), 0, "rejected attempts must never update the state ref");
  assert.equal(backend.childCommitsOf(head), 0, "rejected attempts must publish nothing");
});

Deno.test("prepared to promotion_pending to observing to kept accepts ambiguous promotion reconciliation", async () => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({});
  const adapter = await makeAdapter(backend);
  const publish = async (document: Fixture): Promise<void> => {
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.compareAndSet(head, parseOk(document)), true);
  };
  await publish(stateDocument({ generation: 1, applications: [appState()] }));
  await publish(stateDocument({
    generation: 2,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: preparedTransaction() }],
  }));
  await publish(stateDocument({
    generation: 3,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: promotionPending({ promotion_result: ambiguous() }),
    }],
  }));
  await publish(stateDocument({
    generation: 4,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: observing({ promotion_result: acknowledged() }),
    }],
  }));
  await publish(stateDocument({
    generation: 5,
    applications: [{
      app: "ai-ubq-fi",
      healthy: candidateAttestation(),
      transaction: keptTransaction(),
    }],
  }));
  const final = adapter.readSnapshot().document!.applications[0]!.transaction!;
  assert.equal(final.phase, "kept");
  assert.equal(final.promotion_intent_at, INTENT, "earlier promotion intent evidence persists");
  assert.equal(final.observation_deadline_at, DEADLINE, "the original observation deadline persists");
  assert.equal(final.created_at, CREATED_AT, "the original transaction origin persists");
  assert.equal(final.promotion_result!.kind, "acknowledged");
  assert.equal(final.promotion_result!.http_status, 204, "the ambiguous result reconciled to HTTP 204");
  assert.equal(final.route!.revision_id, "rev-b");
  assert.equal(final.observation.samples, 3);
});

Deno.test("blocked stays nonterminal: no replacement, prepared, or terminal jump, and reason changes are legal", async () => {
  const backend = await seedStateBranch(stateDocument({
    generation: 2,
    applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: blockedTransaction() }],
  }));
  const adapter = await makeAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  const attempts: ReadonlyArray<readonly [string, Fixture, Fixture]> = [
    [
      "a different transaction id may never replace a blocked transaction",
      preparedTransaction({ id: "tx-2", fence_generation: 3 }),
      previousAttestation(),
    ],
    ["blocked may never regress to prepared", preparedTransaction(), previousAttestation()],
    ["blocked may never jump directly to kept", keptTransaction(), candidateAttestation()],
    ["blocked may never jump directly to rolled_back", rolledBackTransaction(), restorationAttestation()],
  ];
  for (const [label, transaction, healthy] of attempts) {
    assert.equal(
      await adapter.compareAndSet(
        head,
        parseOk(stateDocument({
          generation: 3,
          applications: [{ app: "ai-ubq-fi", healthy, transaction }],
        })),
      ),
      false,
      label,
    );
  }
  assert.equal(backend.childCommitsOf(head), 0, "rejected blocked transitions must publish nothing");
  assert.equal(
    countRequestsWhere(backend, "POST"),
    0,
    "rejected blocked transitions must create no Git objects",
  );
  assert.equal(
    countRequestsWhere(backend, "PATCH"),
    0,
    "rejected blocked transitions must never update the state ref",
  );
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 3,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: blockedTransaction({ reason: "updated_dependency_reason" }),
        }],
      })),
    ),
    true,
    "a blocked dependency reason may change while the transaction stays blocked",
  );
  const current = adapter.readSnapshot().document!.applications[0]!.transaction!;
  assert.equal(current.phase, "blocked", "blocked remains nonterminal");
  assert.equal(current.reason, "updated_dependency_reason");
});

Deno.test("blocked resumes exactly the phase implied by its retained promotion/rollback evidence", async () => {
  const stages: ReadonlyArray<
    Readonly<{
      label: string;
      seed: Fixture;
      resume: Fixture;
      phase: string;
      rejected?: Fixture;
    }>
  > = [
    {
      label: "no promotion result and no rollback evidence",
      seed: blockedTransaction({ promotion_result: null }),
      resume: promotionPending(),
      phase: "promotion_pending",
    },
    {
      label: "a recorded promotion result",
      seed: blockedTransaction({ promotion_result: ambiguous() }),
      resume: observing({ promotion_result: ambiguous() }),
      phase: "observing",
    },
    {
      label: "a recorded rollback intent",
      seed: blockedTransaction({
        promotion_result: acknowledged(),
        rollback_intent_at: ROLLBACK_INTENT,
      }),
      resume: rollbackPending({ promotion_result: acknowledged() }),
      phase: "rollback_pending",
      rejected: observing({ promotion_result: acknowledged() }),
    },
    {
      label: "an acknowledged rollback result",
      seed: blockedTransaction({
        promotion_result: acknowledged(),
        rollback_intent_at: ROLLBACK_INTENT,
        rollback_result: { ...acknowledged(), observed_at: ROLLBACK_RESULT_AT },
      }),
      resume: rollbackPendingVerification({ promotion_result: acknowledged() }),
      phase: "rollback_pending_verification",
      rejected: rollbackPending({ promotion_result: acknowledged() }),
    },
  ];
  for (const stage of stages) {
    const backend = await seedStateBranch(stateDocument({
      generation: 2,
      applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: stage.seed }],
    }));
    const adapter = await makeAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    if (stage.rejected !== undefined) {
      assert.equal(
        await adapter.compareAndSet(
          head,
          parseOk(stateDocument({
            generation: 3,
            applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: stage.rejected }],
          })),
        ),
        false,
        `${stage.label}: every phase other than the implied one must be rejected`,
      );
    }
    assert.equal(countRequestsWhere(backend, "POST"), 0, `${stage.label}: no Git object before a valid resume`);
    assert.equal(countRequestsWhere(backend, "PATCH"), 0, `${stage.label}: no ref update before a valid resume`);
    assert.equal(
      await adapter.compareAndSet(
        head,
        parseOk(stateDocument({
          generation: 3,
          applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: stage.resume }],
        })),
      ),
      true,
      `${stage.label}: blocked must resume ${stage.phase}`,
    );
    assert.equal(adapter.readSnapshot().document!.applications[0]!.transaction!.phase, stage.phase);
  }
});

Deno.test("seeded rollback_pending rejects phase jumps, evidence rewrites, and parser-valid clearings without publishing", async () => {
  const cases: ReadonlyArray<Readonly<{ label: string; seed: Fixture; attempt: Fixture }>> = [
    {
      label: "rollback_pending may never jump to observing",
      seed: rollbackPending(),
      attempt: observing({ promotion_result: ambiguous() }),
    },
    {
      label: "promotion_intent_at rewound with its bound deadline",
      seed: rollbackPending(),
      attempt: rollbackPending({
        promotion_intent_at: "2026-09-04T11:30:00.000Z",
        observation_deadline_at: "2026-09-04T12:00:00.000Z",
      }),
    },
    {
      label: "observation_deadline_at rewound with its bound intent",
      seed: rollbackPending(),
      attempt: rollbackPending({
        promotion_intent_at: "2026-09-04T11:45:00.000Z",
        observation_deadline_at: "2026-09-04T12:15:00.000Z",
      }),
    },
    {
      label: "rollback_intent_at rewritten",
      seed: rollbackPending(),
      attempt: rollbackPending({ rollback_intent_at: "2026-09-04T13:30:00.000Z" }),
    },
    {
      label: "promotion_result explicitly cleared on a blocked shape",
      seed: blockedTransaction({ promotion_result: acknowledged(), rollback_intent_at: ROLLBACK_INTENT }),
      attempt: blockedTransaction({ promotion_result: null, rollback_intent_at: ROLLBACK_INTENT }),
    },
    {
      label: "rollback intent explicitly cleared on a blocked shape",
      seed: blockedTransaction({ promotion_result: acknowledged(), rollback_intent_at: ROLLBACK_INTENT }),
      attempt: blockedTransaction({ promotion_result: acknowledged(), rollback_intent_at: null }),
    },
  ];
  for (const entry of cases) {
    const backend = await seedStateBranch(stateDocument({
      generation: 2,
      applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: entry.seed }],
    }));
    const adapter = await makeAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(
      await adapter.compareAndSet(
        head,
        parseOk(stateDocument({
          generation: 3,
          applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction: entry.attempt }],
        })),
      ),
      false,
      entry.label,
    );
    assert.equal(countRequestsWhere(backend, "POST"), 0, `${entry.label}: no Git object may be created`);
    assert.equal(countRequestsWhere(backend, "PATCH"), 0, `${entry.label}: the state ref must never be updated`);
  }
});

Deno.test("newer route and observation evidence truthfully refresh with counter resets and invariant changes", async () => {
  const baselineObservation = {
    last_observed_at: "2026-09-04T12:35:00.000Z",
    samples: 2,
    consecutive_liveness_failures: 2,
    consecutive_inference_failures: 0,
    invariant_id: "fix-1",
    consecutive_invariant_failures: 2,
  };
  const backend = await seedStateBranch(stateDocument({
    generation: 2,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: seededObserving({ observation: baselineObservation }),
    }],
  }));
  const adapter = await makeAdapter(backend);
  const publish = async (generation: number, transaction: Fixture): Promise<void> => {
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(
      await adapter.compareAndSet(
        head,
        parseOk(stateDocument({
          generation,
          applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction }],
        })),
      ),
      true,
      `generation ${generation}`,
    );
  };
  await publish(
    3,
    seededObserving({
      observation: baselineObservation,
      route: { revision_id: "rev-a", observed_at: "2026-09-04T12:41:00.000Z", evidence_ref: "github:route-previous" },
    }),
  );
  await publish(
    4,
    seededObserving({
      observation: baselineObservation,
      route: { revision_id: "rev-b", observed_at: "2026-09-04T12:42:00.000Z", evidence_ref: "github:route-candidate" },
    }),
  );
  await publish(
    5,
    seededObserving({
      route: { revision_id: "rev-b", observed_at: "2026-09-04T12:42:00.000Z", evidence_ref: "github:route-candidate" },
      observation: {
        last_observed_at: "2026-09-04T12:42:00.000Z",
        samples: 3,
        consecutive_liveness_failures: 1,
        consecutive_inference_failures: 0,
        invariant_id: "fix-1",
        consecutive_invariant_failures: 1,
      },
    }),
  );
  await publish(
    6,
    seededObserving({
      route: { revision_id: "rev-b", observed_at: "2026-09-04T12:42:00.000Z", evidence_ref: "github:route-candidate" },
      observation: {
        last_observed_at: "2026-09-04T12:43:00.000Z",
        samples: 4,
        consecutive_liveness_failures: 0,
        consecutive_inference_failures: 0,
        invariant_id: "fix-2",
        consecutive_invariant_failures: 0,
      },
    }),
  );
  const final = adapter.readSnapshot().document!.applications[0]!.transaction!;
  assert.equal(final.route!.revision_id, "rev-b", "the newest truthful route evidence wins");
  assert.equal(final.observation.samples, 4);
  assert.equal(final.observation.consecutive_liveness_failures, 0, "counters may reset to zero with newer samples");
  assert.equal(final.observation.invariant_id, "fix-2", "the invariant identity may change with newer samples");
});

Deno.test("an ambiguous rollback result may reconcile to acknowledged 204 and the acknowledged result is immutable", async () => {
  const backend = await seedStateBranch(stateDocument({
    generation: 2,
    applications: [{
      app: "ai-ubq-fi",
      healthy: previousAttestation(),
      transaction: rollbackPending({
        rollback_result: {
          ...ambiguous(),
          observed_at: ROLLBACK_RESULT_AT,
          evidence_ref: "github:rollback-ambiguous",
        },
      }),
    }],
  }));
  const adapter = await makeAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.compareAndSet(
      head,
      parseOk(stateDocument({
        generation: 3,
        applications: [{
          app: "ai-ubq-fi",
          healthy: previousAttestation(),
          transaction: rollbackPendingVerification(),
        }],
      })),
    ),
    true,
    "ambiguous rollback evidence may resolve to an acknowledged HTTP 204 at an equal observed_at",
  );
  const current = adapter.readSnapshot().commit_sha;
  assert.equal(
    adapter.readSnapshot().document!.applications[0]!.transaction!.rollback_result!.kind,
    "acknowledged",
  );
  assert.equal(countRequestsWhere(backend, "PATCH"), 1, "only the reconciliation may publish");
  const postsAfterReconciliation = countRequestsWhere(backend, "POST");
  const patchesAfterReconciliation = countRequestsWhere(backend, "PATCH");
  const rewrites: ReadonlyArray<readonly [string, Fixture]> = [
    [
      "acknowledged rollback result rewritten with new evidence",
      rollbackPendingVerification({
        rollback_result: {
          ...acknowledged(),
          observed_at: ROLLBACK_RESULT_AT,
          evidence_ref: "github:rollback-rewritten",
        },
      }),
    ],
    [
      "acknowledged rollback result backdated",
      rollbackPendingVerification({
        rollback_result: { ...acknowledged(), observed_at: "2026-09-04T13:04:00.000Z" },
      }),
    ],
    [
      "acknowledged rollback result erased through a phase regression",
      rollbackPending({ rollback_result: null }),
    ],
  ];
  for (const [label, transaction] of rewrites) {
    assert.equal(
      await adapter.compareAndSet(
        current,
        parseOk(stateDocument({
          generation: 4,
          applications: [{ app: "ai-ubq-fi", healthy: previousAttestation(), transaction }],
        })),
      ),
      false,
      label,
    );
  }
  assert.equal(backend.childCommitsOf(current), 0, "rejected rewrites must publish nothing");
  assert.equal(
    countRequestsWhere(backend, "POST"),
    postsAfterReconciliation,
    "rejected rewrites must create no additional Git objects",
  );
  assert.equal(
    countRequestsWhere(backend, "PATCH"),
    patchesAfterReconciliation,
    "rejected rewrites must never update the state ref",
  );
  assert.equal(
    await adapter.compareAndSet(
      current,
      parseOk(stateDocument({
        generation: 4,
        applications: [{ app: "ai-ubq-fi", healthy: restorationAttestation(), transaction: rolledBackTransaction() }],
      })),
    ),
    true,
    "a verified rollback may complete to rolled_back preserving its exact prior evidence",
  );
  const rolledBack = adapter.readSnapshot().document!.applications[0]!.transaction!;
  assert.equal(rolledBack.phase, "rolled_back", "the transaction is terminal after rollback verification");
  assert.equal(rolledBack.rollback_result!.kind, "acknowledged");
  assert.equal(
    rolledBack.rollback_result!.observed_at,
    ROLLBACK_RESULT_AT,
    "the reconciled rollback evidence persists",
  );
  assert.equal(rolledBack.rollback_intent_at, ROLLBACK_INTENT);
  assert.equal(rolledBack.route!.revision_id, "rev-a", "the restored route identifies the previous revision");
  assert.equal(rolledBack.restoration!.git_sha, SHA_A, "the restoration matches the previous identity");
  assert.equal(rolledBack.restoration!.verified_at, RESTORED_AT);
});

// ---------------------------------------------------------------------------
// Provider executor handover: exact run-attempt authority, raw evidence
// retention, and the dedicated owner transition.
// ---------------------------------------------------------------------------

const GITHUB_RUN_URL_BASE = `https://github.com/${REPOSITORY}/actions/runs`;
const REVISION_CONTROL_WORKFLOW = ".github/workflows/sentinel-revision-control.yml";
const HANDOVER_NOW = Date.parse("2026-09-04T11:30:00.000Z");

type AttemptOverrides = Readonly<Record<string, unknown>>;

type AttemptFixture = Readonly<{
  runId: number;
  runAttempt: number;
  payload: Record<string, unknown>;
}>;

const sha256Hex = async (data: Uint8Array): Promise<string> => {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(data)));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const attemptKey = (runId: number, runAttempt: number): string => `/actions/runs/${runId}/attempts/${runAttempt}`;

const setAttempt = (
  backend: FakeGitHubBackend,
  runId: number,
  runAttempt: number,
  factory: () => Response,
): void => {
  backend.attemptResponses.set(attemptKey(runId, runAttempt), factory);
};

const attemptPayload = (
  runId: number,
  runAttempt: number,
  overrides: AttemptOverrides = {},
): Record<string, unknown> => ({
  id: runId,
  run_attempt: runAttempt,
  repository: { full_name: REPOSITORY },
  path: REVISION_CONTROL_WORKFLOW,
  status: "completed",
  conclusion: "success",
  created_at: "2026-09-04T09:00:00Z",
  run_started_at: "2026-09-04T09:30:00Z",
  updated_at: "2026-09-04T10:00:00Z",
  html_url: `${GITHUB_RUN_URL_BASE}/${runId}`,
  ...overrides,
});

const inProgressAttempt = (
  runId: number,
  runAttempt: number,
  overrides: AttemptOverrides = {},
): Record<string, unknown> =>
  attemptPayload(runId, runAttempt, { status: "in_progress", conclusion: null, ...overrides });

const standardHandoverAuthority = (
  retiringRunId = 42,
  retiringRunAttempt = 1,
  nextRunId = 43,
  nextRunAttempt = 1,
  retiringOverrides: AttemptOverrides = {},
  nextOverrides: AttemptOverrides = {},
): { retiring: AttemptFixture; next: AttemptFixture } => ({
  retiring: {
    runId: retiringRunId,
    runAttempt: retiringRunAttempt,
    payload: attemptPayload(retiringRunId, retiringRunAttempt, retiringOverrides),
  },
  next: {
    runId: nextRunId,
    runAttempt: nextRunAttempt,
    payload: inProgressAttempt(nextRunId, nextRunAttempt, nextOverrides),
  },
});

const registerHandoverAuthority = (
  backend: FakeGitHubBackend,
  authority: { retiring: AttemptFixture; next: AttemptFixture },
): void => {
  setAttempt(
    backend,
    authority.retiring.runId,
    authority.retiring.runAttempt,
    () => Response.json(authority.retiring.payload),
  );
  setAttempt(backend, authority.next.runId, authority.next.runAttempt, () => Response.json(authority.next.payload));
};

const revisionControlExecutor = (runId: number, runAttempt: number): Record<string, unknown> => ({
  repository: REPOSITORY,
  workflow_path: REVISION_CONTROL_WORKFLOW,
  run_id: runId,
  run_attempt: runAttempt,
});

const handoverTransaction = (overrides: Record<string, unknown> = {}): Fixture =>
  preparedTransaction({ executor: revisionControlExecutor(42, 1), ...overrides });

const handoverDocument = (
  transaction: Fixture | null = handoverTransaction(),
  generation = 2,
  overrides: Fixture = {},
): Fixture =>
  stateDocument({
    generation,
    applications: [
      { app: "ai-ubq-fi", healthy: previousAttestation(), transaction },
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
    ],
    ...overrides,
  });

const seedHandoverBranch = async (document: Fixture): Promise<FakeGitHubBackend> => {
  const backend = new FakeGitHubBackend();
  await backend.seedDevelopment({
    [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(parseOk(document)),
    "docs/other.txt": "unrelated bytes\n",
  });
  await backend.siblingCreateBranch({});
  return backend;
};

const makeHandoverAdapter = (
  backend: FakeGitHubBackend,
  now: () => number = () => HANDOVER_NOW,
) =>
  createGitHubSentinelProviderState({
    token: "test-token",
    repository: REPOSITORY,
    fetcher: backend.fetcher,
    now,
  });

const attemptRequests = (backend: FakeGitHubBackend): ReadonlyArray<
  Readonly<{
    path: string;
    url: string;
    redirect: string;
  }>
> =>
  backend.requests.filter(
    (request) => request.method === "GET" && request.path.startsWith("/actions/runs/"),
  );

const assertNoPublication = (backend: FakeGitHubBackend, label: string): void => {
  assert.equal(countRequestsWhere(backend, "POST"), 0, `${label} must create no Git objects`);
  assert.equal(countRequestsWhere(backend, "PATCH"), 0, `${label} must never update the state ref`);
};

const assertExactRunAttemptReads = (backend: FakeGitHubBackend, label: string): void => {
  for (const request of attemptRequests(backend)) {
    assert.match(request.path, /^\/actions\/runs\/[0-9]+\/attempts\/[0-9]+$/u, `${label}: only exact endpoints`);
    const url = new URL(request.url);
    assert.equal(url.origin, "https://api.github.com", `${label}: fixed API host only`);
    assert.equal(url.username, "", `${label}: no credentials in the attempt URL`);
    assert.equal(url.search, "", `${label}: no query in the attempt URL`);
    assert.equal(url.hash, "", `${label}: no fragment in the attempt URL`);
    assert.equal(request.redirect, "error", `${label}: redirects are disabled`);
  }
};

const trackedResponseBody = (bytes: Uint8Array, onCancel: () => void): Response => {
  // Keep the stream open after enqueue: the bounded reader must be the one
  // cancelling it (oversize bytes or fatal UTF-8), and the source cancel hook
  // is the only observable of that cancellation.
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes);
    },
    cancel() {
      onCancel();
    },
  });
  return new Response(stream, { status: 200 });
};

const expectedVerifiedAttempt = (
  payload: Record<string, unknown>,
  runId: number,
  runAttempt: number,
  status: string,
  conclusion: string | null,
): Record<string, unknown> => ({
  request_path: `/repos/${REPOSITORY}/actions/runs/${runId}/attempts/${runAttempt}`,
  http_status: 200,
  response: payload,
  run_id: runId,
  run_attempt: runAttempt,
  html_url: `${GITHUB_RUN_URL_BASE}/${runId}`,
  status,
  conclusion,
  created_at: "2026-09-04T09:00:00.000Z",
  run_started_at: "2026-09-04T09:30:00.000Z",
  updated_at: "2026-09-04T10:00:00.000Z",
});

const expectedEvidenceContent = (
  transactionId: string,
  stateCommitSha: string,
  observedAt: string,
  retiring: AttemptFixture,
  next: AttemptFixture,
): string =>
  `${
    JSON.stringify(
      {
        schema_version: 1,
        transaction_id: transactionId,
        state_commit_sha: stateCommitSha,
        observed_at: observedAt,
        retiring: expectedVerifiedAttempt(
          retiring.payload,
          retiring.runId,
          retiring.runAttempt,
          "completed",
          "success",
        ),
        next: expectedVerifiedAttempt(
          next.payload,
          next.runId,
          next.runAttempt,
          "in_progress",
          null,
        ),
      },
      null,
      2,
    )
  }\n`;

const withoutOwnerFields = (transaction: SentinelProviderTransactionV1): Fixture => {
  const copy = { ...transaction } as Record<string, unknown>;
  delete copy.executor;
  delete copy.fence_generation;
  delete copy.retired_executor;
  return copy;
};

Deno.test("handover accepts every real terminal retiring conclusion with an in_progress replacement", async () => {
  const conclusions = [
    "success",
    "failure",
    "cancelled",
    "timed_out",
    "action_required",
    "neutral",
    "skipped",
  ] as const;
  for (const conclusion of conclusions) {
    const backend = await seedHandoverBranch(handoverDocument());
    registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 43, 1, { conclusion }));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(
      await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
      true,
      `conclusion ${conclusion} must be accepted`,
    );
    assertExactRunAttemptReads(backend, `conclusion ${conclusion}`);
    const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
    const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
    assert.equal(document.generation, 3, `conclusion ${conclusion} advances the generation`);
    assert.equal(transaction.fence_generation, 3);
    assert.deepEqual(transaction.executor, revisionControlExecutor(43, 1));
    assert.deepEqual(transaction.retired_executor!.executor, revisionControlExecutor(42, 1));
    assert.equal(transaction.retired_executor!.conclusion, conclusion);
    assert.equal(transaction.retired_executor!.observed_at, "2026-09-04T11:30:00.000Z");
    assert.match(transaction.retired_executor!.evidence_ref, /^sha256:[0-9a-f]{64}$/u);
  }
});

Deno.test("handover accepts a replacement in a newer attempt of the same run", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 42, 2));
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(42, 2)), true);
  const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.deepEqual(transaction.executor, revisionControlExecutor(42, 2));
  assert.equal(transaction.retired_executor!.executor.run_id, 42);
  assert.equal(transaction.retired_executor!.executor.run_attempt, 1);
});

Deno.test("handover fails closed on every response identity, path, repository, id, attempt, and URL mismatch", async () => {
  const mismatches: ReadonlyArray<readonly [string, AttemptOverrides]> = [
    ["response id mismatch", { id: 99 }],
    ["response run_attempt mismatch", { run_attempt: 7 }],
    ["repository full_name mismatch", { repository: { full_name: "other/repo" } }],
    ["repository is not an object", { repository: null }],
    ["workflow path mismatch", { path: ".github/workflows/other.yml" }],
    ["html_url on another host", { html_url: "https://github.com/other/actions/runs/42" }],
    ["html_url for another run", { html_url: `${GITHUB_RUN_URL_BASE}/99` }],
    ["html_url for another attempt", { html_url: `${GITHUB_RUN_URL_BASE}/42/attempts/7` }],
    ["html_url with a query", { html_url: `${GITHUB_RUN_URL_BASE}/42?ref=dev` }],
    ["html_url with a fragment", { html_url: `${GITHUB_RUN_URL_BASE}/42#frag` }],
    ["html_url is not a string", { html_url: 42 }],
  ];
  for (const [label, overrides] of mismatches) {
    const backend = await seedHandoverBranch(handoverDocument());
    registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 43, 1, overrides));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assertNoPublication(backend, label);
    assertExactRunAttemptReads(backend, label);
    assert.equal(attemptRequests(backend).length, 1, `${label}: the retiring read must fail closed`);
    assert.equal(backend.childCommitsOf(head), 0, `${label}: no commit may be created`);
    assert.equal(backend.stateRefHead(), head, `${label}: the state ref must not move`);
  }
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 43, 1, {}, { id: 99 }));
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false);
  assertNoPublication(backend, "replacement identity mismatch");
  assert.equal(attemptRequests(backend).length, 2, "the replacement identity is checked after the retiring read");
});

Deno.test("an in_progress retiring attempt or a completed/concluded replacement is never accepted", async () => {
  const cases: ReadonlyArray<readonly [string, AttemptOverrides, AttemptOverrides]> = [
    ["retiring attempt is in_progress", { status: "in_progress" }, {}],
    ["replacement is a completed historical executor", {}, { status: "completed", conclusion: "success" }],
    ["replacement carries a conclusion", {}, { conclusion: "success" }],
    ["replacement carries a failure conclusion", {}, { conclusion: "failure" }],
  ];
  for (const [label, retiringOverrides, nextOverrides] of cases) {
    const backend = await seedHandoverBranch(handoverDocument());
    registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 43, 1, retiringOverrides, nextOverrides));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assertNoPublication(backend, label);
    assertExactRunAttemptReads(backend, label);
  }
});

Deno.test("a null, unknown, or synthetic stale retiring conclusion is rejected", async () => {
  // Both exact responses are read before status/conclusion semantics; only a
  // non-string conclusion fails inside the per-attempt validation layer and
  // rejects before the replacement read.
  const cases: ReadonlyArray<readonly [string, AttemptOverrides, number]> = [
    ["null conclusion", { conclusion: null }, 2],
    ["unknown conclusion", { conclusion: "unknown" }, 2],
    ["synthetic stale conclusion", { conclusion: "stale" }, 2],
    ["non-string conclusion", { conclusion: 17 }, 1],
  ];
  for (const [label, retiringOverrides, expectedReads] of cases) {
    const backend = await seedHandoverBranch(handoverDocument());
    registerHandoverAuthority(backend, standardHandoverAuthority(42, 1, 43, 1, retiringOverrides));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assertNoPublication(backend, label);
    assert.equal(attemptRequests(backend).length, expectedReads, `${label}: attempt read count`);
  }
});

Deno.test("handover fails closed on malformed evidence, oversize or binary bodies, token echoes, and transport errors", async () => {
  const rows: ReadonlyArray<Readonly<{ label: string; attempts: number; factory: () => Response }>> = [
    {
      label: "malformed created_at",
      attempts: 1,
      factory: () => Response.json(attemptPayload(42, 1, { created_at: "yesterday" })),
    },
    {
      label: "malformed run_started_at",
      attempts: 1,
      factory: () => Response.json(attemptPayload(42, 1, { run_started_at: "no time" })),
    },
    {
      label: "malformed updated_at",
      attempts: 1,
      factory: () => Response.json(attemptPayload(42, 1, { updated_at: 7 })),
    },
    {
      label: "created_at after run_started_at",
      attempts: 1,
      factory: () => Response.json(attemptPayload(42, 1, { created_at: "2026-09-04T09:40:00.000Z" })),
    },
    {
      label: "run_started_at after updated_at",
      attempts: 1,
      factory: () => Response.json(attemptPayload(42, 1, { run_started_at: "2026-09-04T10:40:00.000Z" })),
    },
    {
      label: "updated_at after the final clock",
      attempts: 2,
      factory: () => Response.json(attemptPayload(42, 1, { updated_at: "2026-09-04T12:00:00.000Z" })),
    },
    { label: "body is not JSON", attempts: 1, factory: () => new Response("{ this is not json", { status: 200 }) },
    { label: "body is a JSON array", attempts: 1, factory: () => Response.json([1, 2, 3]) },
    { label: "HTTP 404 status", attempts: 1, factory: () => new Response("not found", { status: 404 }) },
    { label: "HTTP 500 status", attempts: 1, factory: () => new Response("server error", { status: 500 }) },
    {
      label: "transport failure",
      attempts: 1,
      factory: () => {
        throw new Error("network down");
      },
    },
    {
      label: "raw token echo",
      attempts: 1,
      factory: () =>
        new Response(JSON.stringify(attemptPayload(42, 1, { note: "authorization test-token" })), { status: 200 }),
    },
  ];
  for (const row of rows) {
    const backend = await seedHandoverBranch(handoverDocument());
    setAttempt(backend, 42, 1, row.factory);
    setAttempt(backend, 43, 1, () => Response.json(inProgressAttempt(43, 1)));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, row.label);
    assertNoPublication(backend, row.label);
    assertExactRunAttemptReads(backend, row.label);
    assert.equal(attemptRequests(backend).length, row.attempts, `${row.label}: read count`);
  }
  for (
    const [label, bytes] of [
      ["oversize body", new TextEncoder().encode("x".repeat(1024 * 1024 + 1))],
      ["malformed UTF-8 body", new Uint8Array([0x22, 0xff, 0xfe, 0x22])],
    ] as const
  ) {
    const backend = await seedHandoverBranch(handoverDocument());
    let cancelled = false;
    setAttempt(backend, 42, 1, () =>
      trackedResponseBody(bytes, () => {
        cancelled = true;
      }));
    setAttempt(backend, 43, 1, () => Response.json(inProgressAttempt(43, 1)));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assert.equal(cancelled, true, `${label}: the response stream must be cancelled`);
    assertNoPublication(backend, label);
    assertExactRunAttemptReads(backend, label);
  }
});

const assertLocalInputRejected = async (
  label: string,
  invoke: (adapter: Awaited<ReturnType<typeof createGitHubSentinelProviderState>>, head: string) => Promise<boolean>,
  seedDocument: Fixture | "missing" = handoverDocument(),
): Promise<void> => {
  const backend = new FakeGitHubBackend();
  if (seedDocument === "missing") {
    await backend.seedDevelopment({ "docs/other.txt": "unrelated bytes\n" });
  } else {
    await backend.seedDevelopment({
      [SENTINEL_PROVIDER_STATE_PATH]: providerStateJson(parseOk(seedDocument)),
      "docs/other.txt": "unrelated bytes\n",
    });
  }
  await backend.siblingCreateBranch({});
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  const before = backend.requests.length;
  assert.equal(await invoke(adapter, head), false, label);
  assert.equal(countRequestsWhere(backend, "POST"), 0, `${label}: no Git objects`);
  assert.equal(countRequestsWhere(backend, "PATCH"), 0, `${label}: no ref update`);
  assert.ok(
    backend.requests.slice(before).every((request) => !request.path.startsWith("/actions/runs/")),
    `${label}: rejected before any attempt GET`,
  );
};

// Terminal local fixtures keep both application entries, overriding only the
// healthy attestation: the parser requires kept to equal the exact candidate
// and rolled_back to equal the exact restoration, never the default previous.
const terminalSeedDocument = (healthy: Fixture, transaction: Fixture): Fixture =>
  stateDocument({
    generation: 2,
    applications: [
      { app: "ai-ubq-fi", healthy, transaction },
      { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
    ],
  });

Deno.test("stale, missing, terminal, or invalid local inputs reject handover before any attempt GET", async () => {
  const cases: ReadonlyArray<
    Readonly<{
      label: string;
      seed?: Fixture | "missing";
      invoke: (
        adapter: Awaited<ReturnType<typeof createGitHubSentinelProviderState>>,
        head: string,
      ) => Promise<boolean>;
    }>
  > = [
    {
      label: "stale expected SHA",
      invoke: (adapter, _head) => adapter.handover("f".repeat(40), "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "missing provider document",
      seed: "missing",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "unknown application",
      invoke: (adapter, head) => adapter.handover(head, "not-an-app", revisionControlExecutor(43, 1)),
    },
    {
      label: "application has no transaction",
      seed: handoverDocument(null),
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "kept terminal transaction",
      seed: terminalSeedDocument(
        candidateAttestation(),
        keptTransaction({ executor: revisionControlExecutor(42, 1) }),
      ),
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "rolled_back terminal transaction",
      seed: terminalSeedDocument(
        restorationAttestation(),
        rolledBackTransaction({ executor: revisionControlExecutor(42, 1) }),
      ),
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "transaction executor is not the revision-control workflow",
      seed: handoverDocument(preparedTransaction()),
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
    {
      label: "non-string application",
      invoke: (adapter, head) => adapter.handover(head, 7 as unknown as string, revisionControlExecutor(43, 1)),
    },
    {
      label: "next executor is null",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", null),
    },
    {
      label: "next executor is an array",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", []),
    },
    {
      label: "next executor wrong repository",
      invoke: (adapter, head) =>
        adapter.handover(head, "ai-ubq-fi", { ...revisionControlExecutor(43, 1), repository: "other/org" }),
    },
    {
      label: "next executor wrong workflow path",
      invoke: (adapter, head) =>
        adapter.handover(head, "ai-ubq-fi", {
          ...revisionControlExecutor(43, 1),
          workflow_path: ".github/workflows/other.yml",
        }),
    },
    {
      label: "next executor run_id zero",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(0, 1)),
    },
    {
      label: "next executor run_id negative",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(-5, 1)),
    },
    {
      label: "next executor run_id float",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(1.5, 1)),
    },
    {
      label: "next executor run_id string",
      invoke: (adapter, head) =>
        adapter.handover(head, "ai-ubq-fi", revisionControlExecutor("43" as unknown as number, 1)),
    },
    {
      label: "next executor run_attempt zero",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 0)),
    },
    {
      label: "next executor run_attempt float",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 2.5)),
    },
    {
      label: "next executor has extra keys",
      invoke: (adapter, head) =>
        adapter.handover(head, "ai-ubq-fi", { ...revisionControlExecutor(43, 1), extra: true }),
    },
    {
      label: "next executor equals the current owner",
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(42, 1)),
    },
    {
      label: "generation overflow",
      seed: handoverDocument(handoverTransaction(), Number.MAX_SAFE_INTEGER),
      invoke: (adapter, head) => adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    },
  ];
  for (const entry of cases) {
    await assertLocalInputRejected(entry.label, entry.invoke, entry.seed ?? handoverDocument());
  }
});

Deno.test("a blocked transaction remains active and handoverable", async () => {
  const backend = await seedHandoverBranch(
    handoverDocument(blockedTransaction({ executor: revisionControlExecutor(42, 1) })),
  );
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(transaction.phase, "blocked", "handover must preserve the phase");
  assert.equal(transaction.retired_executor!.conclusion, "success");
  assert.deepEqual(transaction.executor, revisionControlExecutor(43, 1));
});

Deno.test("attempt-read drift that moves the remote ref before publication rejects with zero Git writes", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  let drifted = false;
  backend.beforeAttemptGet = async () => {
    if (drifted) return;
    drifted = true;
    await backend.siblingWrite({ "docs/drift.txt": "sibling moved the ref" });
  };
  assert.equal(
    await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    false,
    "the post-authority recheck must reject the drifted parent",
  );
  assertNoPublication(backend, "post-authority ref drift");
  assertExactRunAttemptReads(backend, "post-authority ref drift");
  assert.equal(attemptRequests(backend).length, 2, "both attempt reads complete before the drift is detected");
  assert.equal(backend.stateRefHead() !== head, true, "the sibling write is the surviving state");
  assert.equal(await backend.headFile("docs/drift.txt"), "sibling moved the ref");
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
});

Deno.test("generic compareAndSet still cannot change the transaction owner and never issues attempt GETs", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  const attempts: ReadonlyArray<readonly [string, Fixture]> = [
    [
      "changed run_id",
      stateDocument({
        generation: 3,
        applications: [
          {
            app: "ai-ubq-fi",
            healthy: previousAttestation(),
            transaction: preparedTransaction({ executor: revisionControlExecutor(43, 1) }),
          },
          { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
        ],
      }),
    ],
    [
      "changed workflow path",
      stateDocument({
        generation: 3,
        applications: [
          {
            app: "ai-ubq-fi",
            healthy: previousAttestation(),
            transaction: preparedTransaction({
              executor: { ...revisionControlExecutor(42, 1), workflow_path: ".github/workflows/other.yml" },
            }),
          },
          { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
        ],
      }),
    ],
    [
      "changed run_attempt",
      stateDocument({
        generation: 3,
        applications: [
          {
            app: "ai-ubq-fi",
            healthy: previousAttestation(),
            transaction: preparedTransaction({ executor: revisionControlExecutor(42, 2) }),
          },
          { app: "p-ai-ubq-fi", healthy: previousAttestation(), transaction: null },
        ],
      }),
    ],
  ];
  for (const [label, candidate] of attempts) {
    assert.equal(await adapter.compareAndSet(head, parseOk(candidate)), false, label);
    assertNoPublication(backend, label);
    assert.equal(attemptRequests(backend).length, 0, `${label}: generic CAS never executes attempt GETs`);
  }
  assert.equal(backend.stateRefHead(), head);
});

Deno.test("a valid handover publishes on the exact captured parent and changes only the allowed transaction fields", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const previous = adapter.readSnapshot();
  const head = previous.commit_sha;
  const previousTransaction = previous.document!.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const headCommit = backend.commits.get(backend.stateRefHead()!)!;
  assert.deepEqual(headCommit.parents, [head], "the exact captured parent must be used");
  assert.equal(countRequestsWhere(backend, "POST"), 4, "one blob per retained file, one tree, one commit");
  assert.equal(countRequestsWhere(backend, "PATCH"), 1, "exactly one ref publication");
  const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  assert.equal(document.generation, previous.document!.generation + 1);
  assert.deepEqual(
    document.applications[1],
    previous.document!.applications[1],
    "the unrelated application is untouched",
  );
  const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(transaction.fence_generation, document.generation);
  assert.deepEqual(transaction.executor, revisionControlExecutor(43, 1));
  assert.deepEqual(transaction.retired_executor!.executor, revisionControlExecutor(42, 1));
  assert.equal(transaction.retired_executor!.conclusion, "success");
  assert.equal(transaction.retired_executor!.observed_at, "2026-09-04T11:30:00.000Z");
  assert.match(transaction.retired_executor!.evidence_ref, /^sha256:[0-9a-f]{64}$/u);
  assert.deepEqual(
    withoutOwnerFields(transaction),
    withoutOwnerFields(previousTransaction),
    "every non-owner transaction field must be preserved exactly",
  );
  assert.deepEqual(
    document.applications.find((app) => app.app === "ai-ubq-fi")!.healthy,
    previous.document!.applications.find((app) => app.app === "ai-ubq-fi")!.healthy,
    "the healthy attestation is untouched",
  );
  assert.equal(await backend.headFile("docs/other.txt"), "unrelated bytes\n", "unrelated tree bytes are preserved");
});

Deno.test("handover retains both raw responses content-addressed in the same commit with a matching evidence_ref", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  const authority = standardHandoverAuthority();
  registerHandoverAuthority(backend, authority);
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const observedAt = "2026-09-04T11:30:00.000Z";
  const evidenceContent = expectedEvidenceContent("tx-1", head, observedAt, authority.retiring, authority.next);
  const digest = await sha256Hex(new TextEncoder().encode(evidenceContent));
  const evidencePath = `${SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH}/${digest}.json`;
  assert.equal(
    await backend.headFile(evidencePath),
    evidenceContent,
    "the content-addressed raw evidence is retained verbatim at the head",
  );
  assert.equal(await backend.fileAtRef(evidencePath, head), null, "the evidence rides only the new commit");
  assert.ok(!evidenceContent.includes("test-token"), "no credential is retained in the evidence");
  const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(
    transaction.retired_executor!.evidence_ref,
    `sha256:${digest}`,
    "the evidence_ref is the sha256 of the retained bytes",
  );
});

Deno.test("E1 to E2 to E3 keeps the E1 raw sidecar and normalized record reachable through the exact parent chain", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  const first = standardHandoverAuthority(42, 1, 43, 1);
  registerHandoverAuthority(backend, first);
  const adapter = await makeHandoverAdapter(backend);
  const head0 = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head0, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const head1 = backend.stateRefHead()!;
  const evidence1 = expectedEvidenceContent("tx-1", head0, "2026-09-04T11:30:00.000Z", first.retiring, first.next);
  const digest1 = await sha256Hex(new TextEncoder().encode(evidence1));

  const second = standardHandoverAuthority(43, 1, 44, 1);
  registerHandoverAuthority(backend, second);
  assert.equal(await adapter.handover(head1, "ai-ubq-fi", revisionControlExecutor(44, 1)), true);
  const head2 = backend.stateRefHead()!;
  const evidence2 = expectedEvidenceContent("tx-1", head1, "2026-09-04T11:30:00.000Z", second.retiring, second.next);
  const digest2 = await sha256Hex(new TextEncoder().encode(evidence2));

  assert.deepEqual(backend.commits.get(head1)!.parents, [head0], "E1 to E2 uses the exact parent");
  assert.deepEqual(backend.commits.get(head2)!.parents, [head1], "E2 to E3 uses the exact parent");
  const path1 = `${SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH}/${digest1}.json`;
  const path2 = `${SENTINEL_PROVIDER_EXECUTOR_EVIDENCE_PATH}/${digest2}.json`;
  assert.equal(await backend.fileAtRef(path1, head1), evidence1, "the E1 raw sidecar rides the E1 commit");
  assert.equal(await backend.fileAtRef(path1, head2), evidence1, "the E1 raw sidecar survives into the E2 commit tree");
  assert.equal(await backend.fileAtRef(path2, head2), evidence2, "the E2 raw sidecar rides the E2 commit");

  const head1Document = parseOk(JSON.parse((await backend.fileAtRef(SENTINEL_PROVIDER_STATE_PATH, head1))!));
  const head1Transaction = head1Document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(head1Transaction.retired_executor!.evidence_ref, `sha256:${digest1}`);
  assert.deepEqual(head1Transaction.retired_executor!.executor, revisionControlExecutor(42, 1));
  assert.deepEqual(head1Transaction.executor, revisionControlExecutor(43, 1));

  const head2Document = parseOk(JSON.parse((await backend.fileAtRef(SENTINEL_PROVIDER_STATE_PATH, head2))!));
  const head2Transaction = head2Document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(head2Transaction.retired_executor!.evidence_ref, `sha256:${digest2}`);
  assert.deepEqual(head2Transaction.retired_executor!.executor, revisionControlExecutor(43, 1));
  assert.deepEqual(head2Transaction.executor, revisionControlExecutor(44, 1));
});

Deno.test("a stale expected parent after a successful handover rejects without new reads or writes", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const head0 = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head0, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const head1 = backend.stateRefHead()!;
  const before = backend.requests.length;
  assert.equal(await adapter.handover(head0, "ai-ubq-fi", revisionControlExecutor(44, 1)), false);
  assert.equal(backend.requests.length, before, "a stale expected SHA must fail before any request");
  assert.equal(countRequestsWhere(backend, "POST"), 4, "no additional Git objects");
  assert.equal(countRequestsWhere(backend, "PATCH"), 1, "no additional ref update");
  assert.equal(backend.stateRefHead(), head1, "the state ref must not move");
});

Deno.test("a racing sibling ref update during handover is a conflict that refreshes without blind retry", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  backend.beforeRefUpdate = async () => {
    await backend.siblingWrite({ "docs/from-sibling.txt": "sibling" });
  };
  assert.equal(
    await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    false,
    "the 422 ref conflict is not success",
  );
  assert.equal(backend.refUpdates.filter((update) => update.method === "PATCH").length, 1, "no blind retry");
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 2, "the sibling kept the prior generation");
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
  assert.equal(await backend.headFile("docs/from-sibling.txt"), "sibling");
});

Deno.test("an ambiguous ref response during handover is an explicit error that refresh can reconcile", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await makeHandoverAdapter(backend);
  const head = adapter.readSnapshot().commit_sha;
  backend.refResponseShaOverride = "f".repeat(40);
  await assert.rejects(
    adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    /ambiguous/u,
  );
  assert.equal(backend.refUpdates.filter((update) => update.method === "PATCH").length, 1, "no blind retry");
  const refreshed = await adapter.refresh();
  assert.equal(refreshed.document!.generation, 3, "the remote publication actually advanced the state");
  assert.deepEqual(
    refreshed.document!.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!.executor,
    revisionControlExecutor(43, 1),
    "the reconciled head carries the published successor",
  );
  assert.deepEqual(
    refreshed.document,
    parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!)),
    "the reconciled snapshot is the exact backend head with its evidence",
  );
  assert.equal(refreshed.commit_sha, backend.stateRefHead());
});

Deno.test("a Unicode-escaped synthetic token in either key or value position is rejected after decoding", async () => {
  const escapedFragments: ReadonlyArray<readonly [string, string]> = [
    ["escaped token in a value", '"\\u0074est-token"'],
    ["escaped token in a key", '"\\u0074est-token":"x"'],
  ];
  for (const [label, fragment] of escapedFragments) {
    const backend = await seedHandoverBranch(handoverDocument());
    const raw = `${JSON.stringify(attemptPayload(42, 1)).slice(0, -1)},${fragment}}`;
    assert.ok(!raw.includes("test-token"), `${label}: the raw body must not contain the literal token`);
    setAttempt(backend, 42, 1, () => new Response(raw, { status: 200 }));
    setAttempt(backend, 43, 1, () => Response.json(inProgressAttempt(43, 1)));
    const adapter = await makeHandoverAdapter(backend);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assertNoPublication(backend, label);
    assert.equal(attemptRequests(backend).length, 1, `${label}: the retiring response must reject`);
  }
});

Deno.test("an advancing injected clock succeeds with the final observation authoritative for evidence", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  // The replacement updated_at (11:40) sits between the initial clock (11:30)
  // and the final clock (11:45): only post-read clock handling accepts it.
  registerHandoverAuthority(
    backend,
    standardHandoverAuthority(42, 1, 43, 1, {}, { updated_at: "2026-09-04T11:40:00Z" }),
  );
  const ticks = [
    Date.parse("2026-09-04T11:30:00.000Z"),
    Date.parse("2026-09-04T11:45:00.000Z"),
    Date.parse("2026-09-04T11:45:00.000Z"),
  ];
  let tick = 0;
  const adapter = await makeHandoverAdapter(backend, () => {
    const value = ticks[Math.min(tick, ticks.length - 1)]!;
    tick += 1;
    return value;
  });
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), true);
  const document = parseOk(JSON.parse((await backend.headFile(SENTINEL_PROVIDER_STATE_PATH))!));
  const transaction = document.applications.find((app) => app.app === "ai-ubq-fi")!.transaction!;
  assert.equal(
    transaction.retired_executor!.observed_at,
    "2026-09-04T11:45:00.000Z",
    "the final clock is authoritative",
  );
});

Deno.test("an invalid or regressing injected clock fails closed before any attempt read or at the final sample", async () => {
  const invalidClocks: ReadonlyArray<readonly [string, () => number]> = [
    ["NaN clock", () => Number.NaN],
    ["Infinity clock", () => Number.POSITIVE_INFINITY],
    ["finite out-of-range clock", () => 1e100],
    ["sub-millisecond clock", () => Date.parse("2026-09-04T11:30:00.000Z") + 0.5],
  ];
  for (const [label, now] of invalidClocks) {
    const backend = await seedHandoverBranch(handoverDocument());
    registerHandoverAuthority(backend, standardHandoverAuthority());
    const adapter = await makeHandoverAdapter(backend, now);
    const head = adapter.readSnapshot().commit_sha;
    assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false, label);
    assertNoPublication(backend, label);
    assert.equal(attemptRequests(backend).length, 0, `${label}: invalid clocks reject before any attempt read`);
  }
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const later = Date.parse("2026-09-04T11:45:00.000Z");
  const earlier = Date.parse("2026-09-04T11:30:00.000Z");
  const ticks = [later, earlier];
  let tick = 0;
  const adapter = await makeHandoverAdapter(backend, () => ticks[tick++]!);
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(
    await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)),
    false,
    "a regressing injected clock must fail closed",
  );
  assertNoPublication(backend, "a regressing injected clock must fail closed");
  assert.equal(attemptRequests(backend).length, 2, "the regression is detected after both reads");
});

Deno.test("a noncanonical adapter repository rejects handover before any attempt read or write", async () => {
  const backend = await seedHandoverBranch(handoverDocument());
  registerHandoverAuthority(backend, standardHandoverAuthority());
  const adapter = await createGitHubSentinelProviderState({
    token: "test-token",
    repository: "ubiquity/not-ai-ubq-fi",
    fetcher: backend.fetcher,
    now: () => HANDOVER_NOW,
  });
  const head = adapter.readSnapshot().commit_sha;
  assert.equal(await adapter.handover(head, "ai-ubq-fi", revisionControlExecutor(43, 1)), false);
  assert.equal(attemptRequests(backend).length, 0, "an unapproved adapter repository must never read a run attempt");
  assertNoPublication(backend, "an unapproved adapter repository");
});
