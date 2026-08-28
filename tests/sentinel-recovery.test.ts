import assert from "node:assert/strict";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  parseSentinelReleaseRecord,
} from "../scripts/sentinel/recovery.ts";

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
