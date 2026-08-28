import assert from "node:assert/strict";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  parseSentinelReleaseRecord,
} from "../scripts/sentinel/recovery.ts";
import {
  acquireSentinelRecoveryLease,
  emptySentinelRecoveryLedger,
  nonTerminalSentinelRecoveryRecords,
  parseSentinelRecoveryLedger,
  renderSentinelRecoveryLedger,
  upsertSentinelRecoveryRecord,
} from "../scripts/sentinel/recovery-ledger.ts";

const record = (overrides: Record<string, unknown> = {}) => ({
  schema_version: 1,
  identity: {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    candidate_generation: 1,
  },
  run_id: "33197180235",
  attempt: 1,
  lease_token: "lease-1",
  base_sha: "b".repeat(40),
  phase: "workspace_dirty",
  disposition: "active",
  state_version: 2,
  created_at: "2026-08-28T18:00:00.000Z",
  updated_at: "2026-08-28T18:01:00.000Z",
  candidate_branch: null,
  candidate_sha: null,
  changed_files: ["scripts/sentinel/main.ts"],
  tree_sha: null,
  failure_class: null,
  failure_fingerprint: null,
  artifact_ids: [],
  artifact_digests: [],
  reason: null,
  next_action: "publish checkpoint",
  predecessor: null,
  ...overrides,
});

Deno.test("recovery records require a durable branch and SHA for checkpoint_durable", () => {
  assert.throws(() => parseSentinelRecoveryRecord(record({ phase: "checkpoint_durable" })), /requires a branch/);
  const durable = parseSentinelRecoveryRecord(record({
    phase: "checkpoint_durable",
    candidate_branch: "sentinel/candidate-33197180235-1",
    candidate_sha: "c".repeat(40),
  }));
  assert.equal(durable.disposition, "active");
});

Deno.test("recovery transition validates identity, lease, and monotonic state", () => {
  const previous = parseSentinelRecoveryRecord(record());
  const next = parseSentinelRecoveryRecord(record({ phase: "checkpoint_publishing", state_version: 3 }));
  assert.doesNotThrow(() => assertSentinelRecoveryTransition(previous, next));
  assert.throws(
    () =>
      assertSentinelRecoveryTransition(
        previous,
        parseSentinelRecoveryRecord(record({ phase: "delivered", disposition: "delivered", state_version: 3 })),
      ),
    /Invalid Sentinel recovery transition/,
  );
  assert.throws(
    () =>
      assertSentinelRecoveryTransition(
        previous,
        parseSentinelRecoveryRecord(record({ phase: "checkpoint_publishing", state_version: 4 })),
      ),
    /state version/,
  );
});

Deno.test("release records require immutable stable identity and evidence", () => {
  const release = parseSentinelReleaseRecord({
    schema_version: 1,
    stable_sha: "d".repeat(40),
    candidate_sha: null,
    acceptance_evidence: ["ci:123", "health:revision"],
    activated_at: "2026-08-28T18:00:00.000Z",
    rollback_reason: null,
    generation: 2,
  });
  assert.equal(release.generation, 2);
  assert.throws(() => parseSentinelReleaseRecord({ ...release, stable_sha: "invalid" }), /invalid/);
});

Deno.test("recovery ledger provides bounded create, CAS, lease, and non-terminal lookup", () => {
  const claimed = parseSentinelRecoveryRecord(record({
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
  }));
  const created = upsertSentinelRecoveryRecord(emptySentinelRecoveryLedger(), claimed, null);
  assert.deepEqual(nonTerminalSentinelRecoveryRecords(created), [claimed]);
  const leased = acquireSentinelRecoveryLease(created, {
    identity: claimed.identity,
    owner: "run-1",
    token: "lease-1",
    now: "2026-08-28T18:00:00.000Z",
    expires_at: "2026-08-28T18:10:00.000Z",
  });
  assert.throws(
    () =>
      acquireSentinelRecoveryLease(leased, {
        identity: claimed.identity,
        owner: "run-2",
        token: "lease-2",
        now: "2026-08-28T18:01:00.000Z",
        expires_at: "2026-08-28T18:11:00.000Z",
      }),
    /leased by another owner/u,
  );
  const running = parseSentinelRecoveryRecord(record({
    phase: "implementation_running",
    state_version: 2,
    candidate_branch: null,
    candidate_sha: null,
    updated_at: "2026-08-28T18:02:00.000Z",
  }));
  const updated = upsertSentinelRecoveryRecord(leased, running, 1);
  assert.equal(
    parseSentinelRecoveryLedger(JSON.parse(renderSentinelRecoveryLedger(updated))).records[0]?.state_version,
    2,
  );
  assert.throws(() => upsertSentinelRecoveryRecord(updated, running, 1), /compare-and-swap/u);
});
