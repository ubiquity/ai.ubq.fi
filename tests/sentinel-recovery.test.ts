import assert from "node:assert/strict";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  parseSentinelReleaseRecord,
  sentinelRecoveryCandidateBranch,
} from "../scripts/sentinel/recovery.ts";
import {
  acquireSentinelRecoveryLease,
  emptySentinelRecoveryLedger,
  nonTerminalSentinelRecoveryRecords,
  parseSentinelRecoveryLedger,
  renderSentinelRecoveryLedger,
  resolveSentinelRecoverySelection,
  sentinelRecoveryIdentityKey,
  type SentinelRecoveryLedgerV1,
  upsertSentinelRecoveryRecord,
} from "../scripts/sentinel/recovery-ledger.ts";
import type { SentinelRecoveryRecordV1 } from "../scripts/sentinel/recovery.ts";
import {
  readGitHubSentinelRecoveryLedger,
  writeGitHubSentinelRecoveryLedger,
} from "../scripts/sentinel/recovery-github-store.ts";
import { runSentinelRecoveryPass } from "../scripts/sentinel/recovery-controller.ts";

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

Deno.test("recovery ledger prunes the oldest terminal record before reaching its hard cap", () => {
  // One rejected circuit for one unchanged source revision with 512
  // generations: only the newest terminal decision is protected for exact
  // source eligibility, so the oldest superseded generation is prunable.
  const terminal = Array.from({ length: 512 }, (_, index) =>
    parseSentinelRecoveryRecord(record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "136",
        source_revision: "a".repeat(64),
        candidate_generation: index + 1,
      },
      phase: "rejected",
      disposition: "rejected",
      updated_at: new Date(Date.parse("2026-08-28T18:01:00.000Z") + index).toISOString(),
      reason: "rejected/no_candidate_diff",
      next_action: null,
    })));
  const full = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: terminal });
  const claimed = parseSentinelRecoveryRecord(record({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "513",
      source_revision: "a".repeat(64),
      candidate_generation: 1,
    },
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
  }));
  const pruned = upsertSentinelRecoveryRecord(full, claimed, null);
  assert.equal(pruned.records.length, 512);
  // The newest protected circuit decision survives; the oldest generation of
  // the same unchanged source revision is pruned as superseded evidence.
  assert.equal(
    pruned.records.some((entry) => entry.identity.source_id === "136" && entry.identity.candidate_generation === 1),
    false,
  );
  assert.equal(
    pruned.records.some((entry) => entry.identity.source_id === "136" && entry.identity.candidate_generation === 512),
    true,
  );
  assert.equal(pruned.records.some((entry) => entry.identity.source_id === "513"), true);
  // The retained circuit decision still blocks the unchanged revision.
  const selection = resolveSentinelRecoverySelection({
    ledger: pruned,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-09-01T00:00:00.000Z",
  });
  assert.equal(selection.eligibility.available, false);
  assert.equal(selection.eligibility.reason, "terminal_rejected");
  assert.equal(
    selection.eligibility.blocking_record?.identity.candidate_generation,
    512,
  );
});

Deno.test("recovery ledger fails closed when required terminal circuit records exceed the bound", () => {
  // 512 distinct unchanged issue revisions with terminal decisions cannot
  // retain all protected circuit decisions beside one new active claim; the
  // ledger must fail closed instead of dropping a terminal decision.
  const terminal = Array.from({ length: 512 }, (_, index) =>
    parseSentinelRecoveryRecord(record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: String(index + 1),
        source_revision: "a".repeat(64),
        candidate_generation: 1,
      },
      phase: "rejected",
      disposition: "rejected",
      updated_at: new Date(Date.parse("2026-08-28T18:01:00.000Z") + index).toISOString(),
      reason: "rejected/no_candidate_diff",
      next_action: null,
    })));
  const full = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: terminal });
  const claimed = parseSentinelRecoveryRecord(record({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "513",
      source_revision: "a".repeat(64),
      candidate_generation: 1,
    },
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
  }));
  assert.throws(
    () => upsertSentinelRecoveryRecord(full, claimed, null),
    /cannot retain required terminal circuit decisions/u,
  );
});

const retryDecision = (
  identity: SentinelRecoveryRecordV1["identity"],
  retryAt: string,
): Record<string, unknown> => ({
  disposition: "retry_wait",
  should_retry: true,
  circuit_open: false,
  validation_repair_allowed: false,
  source_revision_changed: false,
  candidate_generation: identity.candidate_generation,
  attempt_count: 1,
  identical_failure_count: 1,
  backoff_ms: 60_000,
  retry_at: retryAt,
  failure_class: "runner_interruption",
  failure_fingerprint: "f".repeat(64),
  reason: "The bounded Sentinel retry policy scheduled another attempt.",
  next_action: "Retry after the scheduled delay.",
});

Deno.test("terminal manual_required with later rejected generations blocks issue137 unchanged source", () => {
  // Issue 137: generation 1 opened the circuit after identical runner
  // interruptions; generations 2-6 restarted the identical source revision
  // and were rejected; generation 7 is active on the same fingerprint.
  const generation = (index: number): unknown =>
    record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "137",
        source_revision: "d".repeat(64),
        candidate_generation: index,
      },
      run_id: `run-${index}`,
      lease_token: `lease-${index}`,
      attempt: index,
      phase: index === 1 ? "manual_required" : index === 7 ? "claimed" : "rejected",
      disposition: index === 1 ? "manual_required" : index === 7 ? "active" : "rejected",
      state_version: index * 10,
      updated_at: new Date(Date.parse("2026-08-28T18:00:00.000Z") + index * 1_000).toISOString(),
      reason: index === 1 ? "Three identical Sentinel failure fingerprints opened the circuit breaker." : null,
      next_action: index === 1 ? "Owner review is required before another Sentinel attempt." : null,
    });
  const records = Array.from({ length: 7 }, (_, index) => parseSentinelRecoveryRecord(generation(index + 1)));
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records });
  const selection = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "137",
    source_revision: "d".repeat(64),
    now: "2026-09-06T14:10:00.000Z",
  });
  // The active generation 7 is in flight, so the exact record is the current
  // one, but the durable terminal circuit still blocks any fresh generation.
  assert.equal(selection.current_record?.identity.candidate_generation, 7);
  assert.equal(selection.retry_is_due, false);
  assert.equal(selection.eligibility.available, false);
  assert.equal(selection.eligibility.reason, "terminal_rejected");
  assert.equal(selection.eligibility.blocking_record?.identity.candidate_generation, 6);
  assert.equal(selection.next_generation, 8);
});

Deno.test("a lone manual_required decision blocks an unchanged source revision", () => {
  const manual = parseSentinelRecoveryRecord(record({
    phase: "manual_required",
    disposition: "manual_required",
    state_version: 9,
    updated_at: "2026-08-28T18:05:00.000Z",
  }));
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [manual] });
  const selection = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(selection.eligibility.available, false);
  assert.equal(selection.eligibility.reason, "terminal_manual_required");
  assert.equal(selection.eligibility.blocking_record?.identity.candidate_generation, 1);
});

Deno.test("a delivered source revision blocks a new generation and a changed revision stays eligible", () => {
  const delivered = parseSentinelRecoveryRecord(record({
    phase: "delivered",
    disposition: "delivered",
    state_version: 9,
    updated_at: "2026-08-28T18:05:00.000Z",
    candidate_branch: "sentinel/candidate-123456789",
    candidate_sha: "c".repeat(40),
    reason: "delivered",
    next_action: null,
  }));
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [delivered] });
  const unchanged = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(unchanged.eligibility.available, false);
  assert.equal(unchanged.eligibility.reason, "terminal_delivered");
  // A changed source revision is a truly unseen revision: eligible again.
  const changed = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "b".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(changed.eligibility.available, true);
  assert.equal(changed.eligibility.reason, "fresh_source");
  assert.equal(changed.next_generation, 2);
});

Deno.test("a rejected unchanged source is unavailable and rejection never resets attempts", () => {
  const records = [1, 2].map((generation) =>
    parseSentinelRecoveryRecord(record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "136",
        source_revision: "a".repeat(64),
        candidate_generation: generation,
      },
      phase: "rejected",
      disposition: "rejected",
      updated_at: new Date(Date.parse("2026-08-28T18:00:00.000Z") + generation * 1_000).toISOString(),
      reason: "rejected/no_candidate_diff",
      next_action: null,
    }))
  );
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records });
  const selection = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(selection.eligibility.available, false);
  assert.equal(selection.eligibility.reason, "terminal_rejected");
  // A future owner-orchestrated changed source continues from generation max+1
  // instead of resetting to generation 1.
  assert.equal(selection.next_generation, 3);
});

Deno.test("an active record proceeds only through its own due retry decision", () => {
  const identity = {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    candidate_generation: 1,
  };
  const waiting = parseSentinelRecoveryRecord(record({
    phase: "retry_wait",
    updated_at: "2026-08-28T18:05:00.000Z",
  }));
  const decision = retryDecision(waiting.identity, "2026-08-28T18:15:00.000Z");
  const ledger = parseSentinelRecoveryLedger({
    ...emptySentinelRecoveryLedger(),
    records: [waiting],
    retry_decisions: [{ identity_key: sentinelRecoveryIdentityKey(waiting.identity), decision }],
  });
  const beforeDue = resolveSentinelRecoverySelection({
    ledger,
    repository: identity.repository,
    source_kind: "github_issue",
    source_id: identity.source_id,
    source_revision: identity.source_revision,
    now: "2026-08-28T18:10:00.000Z",
  });
  assert.equal(beforeDue.eligibility.available, false);
  assert.equal(beforeDue.eligibility.reason, "active_unavailable");
  assert.equal(beforeDue.eligibility.blocking_record, waiting);
  assert.equal(beforeDue.retry_is_due, false);
  const due = resolveSentinelRecoverySelection({
    ledger,
    repository: identity.repository,
    source_kind: "github_issue",
    source_id: identity.source_id,
    source_revision: identity.source_revision,
    now: "2026-08-28T18:16:00.000Z",
  });
  assert.equal(due.eligibility.available, true);
  assert.equal(due.eligibility.reason, "active_retry_due");
  assert.equal(due.current_record, waiting);
  assert.equal(due.retry_is_due, true);
  // An in-flight active record without any retry decision is unavailable too.
  const running = parseSentinelRecoveryRecord(record({
    phase: "checkpoint_durable",
    candidate_branch: "sentinel/candidate-123456789",
    candidate_sha: "c".repeat(40),
  }));
  const runningLedger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [running] });
  const blocked = resolveSentinelRecoverySelection({
    ledger: runningLedger,
    repository: identity.repository,
    source_kind: "github_issue",
    source_id: identity.source_id,
    source_revision: identity.source_revision,
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(blocked.eligibility.available, false);
  assert.equal(blocked.eligibility.reason, "active_unavailable");
  assert.equal(blocked.current_record, running);
});

Deno.test("an unseen source revision remains eligible and keeps the family next generation", () => {
  const manual = parseSentinelRecoveryRecord(record({
    phase: "manual_required",
    disposition: "manual_required",
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "136",
      source_revision: "a".repeat(64),
      candidate_generation: 3,
    },
    state_version: 9,
    updated_at: "2026-08-28T18:05:00.000Z",
    reason: "manual",
    next_action: null,
  }));
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [manual] });
  const unseen = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "b".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(unseen.eligibility.available, true);
  assert.equal(unseen.eligibility.reason, "fresh_source");
  assert.equal(unseen.next_generation, 4);
  assert.equal(unseen.current_record, null);
  const empty = resolveSentinelRecoverySelection({
    ledger: emptySentinelRecoveryLedger(),
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "review_backlog",
    source_id: "f".repeat(64),
    source_revision: "b".repeat(40),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(empty.eligibility.available, true);
  assert.equal(empty.eligibility.reason, "fresh_source");
  assert.equal(empty.next_generation, 1);
});

Deno.test("matrix convergence may continue its own prepared exact recovery record", () => {
  // The prepare run's claimed record shares the unchanged source revision with
  // older terminal decisions; the convergence run must not see itself as a
  // competing fresh claim for the run's own exact active record.
  const prepared = parseSentinelRecoveryRecord(record({
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue",
      source_id: "137",
      source_revision: "d".repeat(64),
      candidate_generation: 7,
    },
    phase: "claimed",
    state_version: 1,
    candidate_branch: null,
    candidate_sha: null,
    run_id: "prepare-run",
    lease_token: "prepare-lease",
    attempt: 2,
    updated_at: "2026-08-28T18:20:00.000Z",
  }));
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records: [prepared] });
  const continuation = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "137",
    source_revision: "d".repeat(64),
    now: "2026-08-28T18:30:00.000Z",
    continuation_record: prepared,
  });
  assert.equal(continuation.eligibility.available, true);
  assert.equal(continuation.eligibility.reason, "active_continuation");
  assert.equal(continuation.current_record, prepared);
  // Continuation is caller-bound to the run identity: a record with the same
  // source identity but a different run_id/lease_token never authorizes it.
  const impostor = parseSentinelRecoveryRecord({
    ...prepared,
    run_id: "other-run",
    lease_token: "other-lease",
  });
  const competitor = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "137",
    source_revision: "d".repeat(64),
    now: "2026-08-28T18:30:00.000Z",
    continuation_record: impostor,
  });
  assert.equal(competitor.eligibility.available, false);
  assert.equal(competitor.eligibility.reason, "active_unavailable");
  // Any other run has no continuation record and defers to the active claim.
  const noContinuation = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "137",
    source_revision: "d".repeat(64),
    now: "2026-08-28T18:30:00.000Z",
  });
  assert.equal(noContinuation.eligibility.available, false);
  assert.equal(noContinuation.eligibility.reason, "active_unavailable");
});

Deno.test("multiple active generations for one exact source fail closed as a collision", () => {
  const records = [1, 2].map((generation) =>
    parseSentinelRecoveryRecord(record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "137",
        source_revision: "d".repeat(64),
        candidate_generation: generation,
      },
      phase: "claimed",
      state_version: 1,
      candidate_branch: null,
      candidate_sha: null,
      run_id: `run-${generation}`,
      lease_token: `lease-${generation}`,
      attempt: 1,
      updated_at: new Date(Date.parse("2026-08-28T18:00:00.000Z") + generation * 1_000).toISOString(),
    }))
  );
  const ledger = parseSentinelRecoveryLedger({ ...emptySentinelRecoveryLedger(), records });
  const selection = resolveSentinelRecoverySelection({
    ledger,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "137",
    source_revision: "d".repeat(64),
    now: "2026-08-28T19:00:00.000Z",
  });
  assert.equal(selection.eligibility.available, false);
  assert.equal(selection.eligibility.reason, "active_collision");
  assert.equal(selection.current_record?.identity.candidate_generation, 2);
});

Deno.test("a live lease from another owner holds a due retry until it expires", () => {
  const waiting = parseSentinelRecoveryRecord(record({
    phase: "retry_wait",
    state_version: 5,
    updated_at: "2026-08-28T18:05:00.000Z",
  }));
  const identityKey = sentinelRecoveryIdentityKey(waiting.identity);
  const decision = retryDecision(waiting.identity, "2026-08-28T18:15:00.000Z");
  const base = {
    ...emptySentinelRecoveryLedger(),
    records: [waiting],
    retry_decisions: [{ identity_key: identityKey, decision }],
  };
  const held = parseSentinelRecoveryLedger({
    ...base,
    leases: [{
      identity_key: identityKey,
      owner: "recovery-controller",
      token: "controller-lease",
      expires_at: "2026-08-28T18:40:00.000Z",
    }],
  });
  const heldSelection = resolveSentinelRecoverySelection({
    ledger: held,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T18:20:00.000Z",
  });
  assert.equal(heldSelection.eligibility.available, false);
  assert.equal(heldSelection.eligibility.reason, "active_lease_held");
  // Once the lease expires the due retry is claimable again.
  const expired = parseSentinelRecoveryLedger({
    ...base,
    leases: [{
      identity_key: identityKey,
      owner: "recovery-controller",
      token: "controller-lease",
      expires_at: "2026-08-28T18:10:00.000Z",
    }],
  });
  const dueAfterExpiry = resolveSentinelRecoverySelection({
    ledger: expired,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T18:20:00.000Z",
  });
  assert.equal(dueAfterExpiry.eligibility.available, true);
  assert.equal(dueAfterExpiry.eligibility.reason, "active_retry_due");
  // The run's own lease token never blocks its own continuation.
  const ownLease = parseSentinelRecoveryLedger({
    ...base,
    leases: [{
      identity_key: identityKey,
      owner: "prepare-run",
      token: waiting.lease_token,
      expires_at: "2026-08-28T18:40:00.000Z",
    }],
  });
  const own = resolveSentinelRecoverySelection({
    ledger: ownLease,
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    now: "2026-08-28T18:20:00.000Z",
    continuation_record: waiting,
  });
  assert.equal(own.eligibility.available, true);
  assert.equal(own.eligibility.reason, "active_continuation");
});

Deno.test("recovery eligibility fails closed on malformed ledger data", () => {
  const malformed: unknown = {
    ...emptySentinelRecoveryLedger(),
    records: [{
      ...record({}),
      phase: "rejected",
      disposition: "active",
    }],
  };
  assert.throws(
    () =>
      resolveSentinelRecoverySelection({
        ledger: malformed,
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "136",
        source_revision: "a".repeat(64),
        now: "2026-08-28T19:00:00.000Z",
      }),
    /ledger is invalid|record is invalid|phase and disposition disagree/u,
  );
  assert.throws(
    () =>
      resolveSentinelRecoverySelection({
        ledger: emptySentinelRecoveryLedger(),
        repository: "",
        source_kind: "github_issue",
        source_id: "136",
        source_revision: "a".repeat(64),
        now: "2026-08-28T19:00:00.000Z",
      }),
    /input is invalid/u,
  );
});

Deno.test("GitHub recovery state uses a separate fast-forward-only ref", async () => {
  const base = "a".repeat(40);
  const baseTree = "b".repeat(40);
  const blob = "c".repeat(40);
  const nextTree = "d".repeat(40);
  const nextCommit = "e".repeat(40);
  const ledgerText = renderSentinelRecoveryLedger(emptySentinelRecoveryLedger());
  const encoded = btoa(ledgerText);
  const requests: Array<{ url: string; init: RequestInit }> = [];
  const responses = [
    new Response("not found", { status: 404 }),
    Response.json({ object: { sha: base } }),
    Response.json({ tree: { sha: baseTree } }),
    Response.json({ encoding: "base64", content: encoded }),
    Response.json({ sha: blob }),
    Response.json({ sha: nextTree }),
    Response.json({ sha: nextCommit }),
    Response.json({ ref: "refs/heads/sentinel/recovery-state", object: { sha: nextCommit } }),
  ];
  const fetcher: typeof fetch = (url, init = {}) => {
    requests.push({ url: String(url), init });
    const response = responses.shift();
    if (!response) throw new Error("Unexpected GitHub recovery-state request");
    return Promise.resolve(response);
  };
  const snapshot = await readGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    fetcher,
  });
  assert.equal(snapshot.state_ref_exists, false);
  const written = await writeGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    snapshot,
    ledger: snapshot.ledger,
    message: "chore(sentinel): claim recovery work",
    fetcher,
  });
  assert.equal(written.commit_sha, nextCommit);
  assert.equal(requests.at(-1)?.url.endsWith("/git/refs"), true);
  assert.deepEqual(JSON.parse(String(requests.at(-1)?.init.body)), {
    ref: "refs/heads/sentinel/recovery-state",
    sha: nextCommit,
  });
});

Deno.test("GitHub recovery state fetches the exact blob for an over-1-MiB contents response", async () => {
  const commit = "a".repeat(40);
  const tree = "b".repeat(40);
  const blobSha = "c".repeat(40);
  // Above 1 MiB the Contents API returns valid file metadata only
  // (`content: ""`, `encoding: "none"`) and keeps the payload in the Git Data
  // blob the validated SHA pins; this mirrors the live recovery branch.
  const ledger = parseSentinelRecoveryLedger({
    schema_version: 1,
    records: [parseSentinelRecoveryRecord(record({ reason: "r".repeat(1_100_000) }))],
    retry_history: [],
    retry_decisions: [],
    leases: [],
  });
  const ledgerText = renderSentinelRecoveryLedger(ledger);
  const size = new TextEncoder().encode(ledgerText).byteLength;
  assert.ok(size > 1_048_576);
  const responses = [
    Response.json({ ref: "refs/heads/sentinel/recovery-state", object: { sha: commit } }),
    Response.json({ sha: commit, tree: { sha: tree } }),
    Response.json({ type: "file", encoding: "none", content: "", size, sha: blobSha }),
    Response.json({ sha: blobSha, size, encoding: "base64", content: btoa(ledgerText) }),
  ];
  const requests: Array<{ url: string }> = [];
  const fetcher: typeof fetch = (url) => {
    requests.push({ url: String(url) });
    const response = responses.shift();
    if (!response) throw new Error("Unexpected large recovery-state request");
    return Promise.resolve(response);
  };
  const snapshot = await readGitHubSentinelRecoveryLedger({
    token: "token",
    repository: "ubiquity/ai.ubq.fi",
    fetcher,
  });
  assert.equal(snapshot.state_ref_exists, true);
  assert.equal(snapshot.commit_sha, commit);
  assert.equal(snapshot.tree_sha, tree);
  assert.deepEqual(snapshot.ledger, ledger);
  assert.equal(requests.length, 4);
  assert.equal(requests[2]?.url.endsWith(`/contents/docs/sentinel-recovery-records.json?ref=${commit}`), true);
  assert.equal(requests[3]?.url.endsWith(`/git/blobs/${blobSha}`), true);
});

Deno.test("GitHub recovery state fails closed on malformed large-file blob responses", async () => {
  const commit = "1".repeat(40);
  const tree = "2".repeat(40);
  const blobSha = "3".repeat(40);
  const encoded = btoa(renderSentinelRecoveryLedger(emptySentinelRecoveryLedger()));
  const contents = { type: "file", encoding: "none", content: "", size: 1_049_697, sha: blobSha };
  const readWith = (contentsValue: unknown, blob: unknown) => {
    const responses = [
      Response.json({ object: { sha: commit } }),
      Response.json({ tree: { sha: tree } }),
      Response.json(contentsValue),
      Response.json(blob),
    ];
    const fetcher: typeof fetch = () => {
      const response = responses.shift();
      if (!response) throw new Error("Unexpected malformed large-file response");
      return Promise.resolve(response);
    };
    return readGitHubSentinelRecoveryLedger({ token: "token", repository: "ubiquity/ai.ubq.fi", fetcher });
  };
  // A blob whose SHA does not match the validated file SHA is not the exact
  // blob the contents metadata identified and must never be accepted.
  await assert.rejects(
    readWith(contents, { sha: "4".repeat(40), encoding: "base64", content: encoded }),
    /file is invalid/u,
  );
  // The Git Data blob must carry base64 content; metadata-only or
  // non-base64 payloads are malformed and fail closed.
  await assert.rejects(readWith(contents, { sha: blobSha, encoding: "base64" }), /file is invalid/u);
  await assert.rejects(readWith(contents, { sha: blobSha, encoding: "none", content: "" }), /file is invalid/u);
  await assert.rejects(
    readWith(contents, { sha: "not-a-sha", encoding: "base64", content: encoded }),
    /blob SHA is invalid/u,
  );
  // Contents metadata without a valid full SHA cannot pin a blob at all.
  await assert.rejects(
    readWith({ ...contents, sha: "not-a-sha" }, { sha: blobSha, encoding: "base64", content: encoded }),
    /file SHA is invalid/u,
  );
});

Deno.test("an empty durable recovery pass performs no state write", async () => {
  const commit = "1".repeat(40);
  const tree = "2".repeat(40);
  const encoded = btoa(renderSentinelRecoveryLedger(emptySentinelRecoveryLedger()));
  const methods: string[] = [];
  const responses = [
    Response.json({ object: { sha: commit } }),
    Response.json({ tree: { sha: tree } }),
    Response.json({ encoding: "base64", content: encoded }),
  ];
  const fetcher: typeof fetch = (_url, init = {}) => {
    methods.push(init.method ?? "GET");
    const response = responses.shift();
    if (!response) throw new Error("Unexpected recovery pass request");
    return Promise.resolve(response);
  };
  assert.deepEqual(
    await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    }),
    [],
  );
  assert.deepEqual(methods, ["GET", "GET", "GET"]);
});

type RecoveryPassDelivery = Readonly<{
  number: number;
  head_branch: string;
  head_sha: string;
  state: "open" | "closed";
  merged_at: string | null;
}>;

/**
 * Minimal stateful GitHub API double for the recovery pass: it answers the
 * ledger state ref, development ref, exact candidate refs, and pull-request
 * observations with one mutable ledger snapshot.
 */
const recoveryPassFetcher = (
  input: Readonly<{
    ledger: SentinelRecoveryLedgerV1;
    candidateShas: Readonly<Record<string, string>>;
    deliveries?: Readonly<Record<string, readonly RecoveryPassDelivery[]>>;
  }>,
): Readonly<{ fetcher: typeof fetch; ledger: () => SentinelRecoveryLedgerV1 }> => {
  const developmentSha = "1".repeat(40);
  let stateSha: string | null = null;
  const blobSha = "2".repeat(40);
  const treeSha = "3".repeat(40);
  let commitCounter = 1;
  let currentLedger = parseSentinelRecoveryLedger(input.ledger);
  const nextCommitSha = (): string => (++commitCounter).toString(16).padStart(40, "0");
  const fetcher: typeof fetch = (inputValue, init = {}) => {
    const url = new URL(String(inputValue));
    const path = url.pathname.replace(/^\/repos\/[^/]+\/[^/]+/u, "");
    if (path === "/git/ref/heads/sentinel/recovery-state") {
      return Promise.resolve(
        stateSha === null
          ? new Response("not found", { status: 404 })
          : Response.json({ ref: path, object: { sha: stateSha } }),
      );
    }
    if (path === "/git/ref/heads/development") {
      return Promise.resolve(Response.json({ ref: path, object: { sha: developmentSha } }));
    }
    const branch = path.match(/^\/git\/ref\/heads\/(sentinel\/candidate-.+)$/u)?.[1];
    if (branch) {
      const sha = input.candidateShas[branch];
      return Promise.resolve(
        sha === undefined ? new Response("not found", { status: 404 }) : Response.json({ ref: path, object: { sha } }),
      );
    }
    const commit = path.match(/^\/git\/commits\/([0-9a-f]{40})$/u);
    if (commit) return Promise.resolve(Response.json({ sha: commit[1], tree: { sha: treeSha } }));
    const contents = path.match(/^\/contents\/docs\/sentinel-recovery-records\.json$/u);
    if (contents) {
      return Promise.resolve(Response.json({
        encoding: "base64",
        content: btoa(renderSentinelRecoveryLedger(currentLedger)),
      }));
    }
    if (path === "/git/blobs") {
      const body = JSON.parse(String(init.body ?? "{}")) as { content?: unknown };
      if (typeof body.content === "string") {
        currentLedger = parseSentinelRecoveryLedger(JSON.parse(body.content));
      }
      return Promise.resolve(Response.json({ sha: blobSha }));
    }
    if (path === "/git/trees") {
      return Promise.resolve(Response.json({ sha: treeSha }));
    }
    if (path === "/git/commits") {
      return Promise.resolve(Response.json({ sha: nextCommitSha(), tree: { sha: treeSha } }));
    }
    if (path === "/git/refs") {
      stateSha = String((JSON.parse(String(init.body)) as { sha?: unknown }).sha ?? stateSha);
      return Promise.resolve(Response.json({ ref: path, object: { sha: stateSha } }));
    }
    if (path === "/git/refs/heads/sentinel/recovery-state") {
      stateSha = String((JSON.parse(String(init.body)) as { sha?: unknown }).sha ?? stateSha);
      return Promise.resolve(Response.json({ ref: path, object: { sha: stateSha } }));
    }
    const pulls = path.match(/^\/pulls$/u) && url.searchParams.get("base") === "development";
    if (pulls) {
      const head = url.searchParams.get("head")?.split(":", 2)[1] ?? null;
      return Promise.resolve(Response.json((input.deliveries?.[head ?? ""] ?? []).map((delivery) => ({
        number: delivery.number,
        state: delivery.state,
        merged_at: delivery.merged_at,
        head: { ref: delivery.head_branch, sha: delivery.head_sha },
        base: { ref: "development" },
      }))));
    }
    if (path.startsWith("/compare/development...")) {
      return Promise.resolve(Response.json({ status: "behind" }));
    }
    throw new Error(`Unexpected recovery pass request ${path}`);
  };
  return { fetcher, ledger: () => parseSentinelRecoveryLedger(currentLedger) };
};

const failureEvidenceRecord = (
  sourceKind: "triage" | "github_issue" = "triage",
): Readonly<{ record: SentinelRecoveryRecordV1; branchSha: string }> => {
  const sourceId = sourceKind === "triage" ? "triage-cell-1" : "208";
  const sourceRevision = "a".repeat(64);
  const candidateSha = "c".repeat(40);
  const parsed = parseSentinelRecoveryRecord({
    ...record({
      identity: {
        repository: "ubiquity/ai.ubq.fi",
        source_kind: sourceKind,
        source_id: sourceId,
        source_revision: sourceRevision,
        candidate_generation: 1,
      },
      lease_token: "lease-recovered",
      phase: "checkpoint_durable",
      state_version: 7,
      created_at: "2026-08-28T17:00:00.000Z",
      updated_at: "2026-08-28T17:05:00.000Z",
      candidate_branch: "sentinel/candidate-unused",
      candidate_sha: candidateSha,
      changed_files: ["candidate.txt"],
      tree_sha: "d".repeat(40),
      failure_class: "runner_interruption",
      failure_fingerprint: "8".repeat(64),
      reason: "encrypted candidate recovered into a quarantined checkpoint",
      next_action: "validate the checkpoint and request human review",
    }),
  });
  return { record: parsed, branchSha: candidateSha };
};

Deno.test({
  name: "a recovered durable checkpoint is reconciled to resume_validation without re-entering retry_wait",
  async fn() {
    const { record: child, branchSha } = failureEvidenceRecord();
    const deterministic = sentinelRecoveryCandidateBranch(child.identity);
    const durable: SentinelRecoveryRecordV1 = parseSentinelRecoveryRecord({
      ...child,
      candidate_branch: deterministic,
    });
    const key = sentinelRecoveryIdentityKey(durable.identity);
    const history = {
      schema_version: 1 as const,
      identity: durable.identity,
      attempt: 1,
      failure_class: "runner_interruption" as const,
      failure_fingerprint: "8".repeat(64),
      observed_at: "2026-08-28T17:00:00.000Z",
    };
    const decision = {
      disposition: "retry_wait" as const,
      should_retry: true,
      circuit_open: false,
      validation_repair_allowed: false,
      source_revision_changed: false,
      candidate_generation: 1,
      attempt_count: 1,
      identical_failure_count: 1,
      backoff_ms: 60_000,
      retry_at: "2026-08-28T17:01:00.000Z",
      failure_class: "runner_interruption" as const,
      failure_fingerprint: "8".repeat(64),
      reason: "The bounded Sentinel retry policy scheduled another attempt.",
      next_action: "Retry after 2026-08-28T17:01:00.000Z.",
    };
    const { fetcher, ledger } = recoveryPassFetcher({
      ledger: {
        schema_version: 1,
        records: [durable],
        retry_history: [history],
        retry_decisions: [{ identity_key: key, decision }],
        leases: [],
      },
      candidateShas: { [deterministic]: branchSha },
    });
    const results = await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    });
    assert.deepEqual(results.map((result) => result.action), ["resume_validation"]);
    const after = ledger();
    const reconciled = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key)!;
    // The successfully reconstructed checkpoint stays durable and never
    // re-enters retry_wait, while the original classified failure evidence
    // remains preserved on the record and in the bounded retry metadata.
    assert.equal(reconciled.phase, "checkpoint_durable");
    assert.equal(reconciled.disposition, "active");
    assert.equal(reconciled.state_version, durable.state_version);
    assert.equal(reconciled.candidate_sha, branchSha);
    assert.equal(reconciled.failure_class, "runner_interruption");
    assert.equal(reconciled.failure_fingerprint, "8".repeat(64));
    assert.equal(after.retry_history.length, 1);
    assert.equal(after.retry_history[0]!.failure_class, "runner_interruption");
    assert.equal(after.retry_decisions.length, 1);
    assert.equal(after.retry_decisions[0]!.identity_key, key);
    assert.equal(after.retry_decisions[0]!.decision.disposition, "retry_wait");
  },
});

Deno.test({
  name: "a recovered checkpoint with a merged delivery progresses to review instead of retry_wait",
  async fn() {
    const { record, branchSha } = failureEvidenceRecord("github_issue");
    const deterministic = sentinelRecoveryCandidateBranch(record.identity);
    const durable: SentinelRecoveryRecordV1 = parseSentinelRecoveryRecord({
      ...record,
      candidate_branch: deterministic,
      candidate_sha: branchSha,
    });
    const key = sentinelRecoveryIdentityKey(durable.identity);
    const { fetcher, ledger } = recoveryPassFetcher({
      ledger: {
        schema_version: 1,
        records: [durable],
        retry_history: [],
        retry_decisions: [],
        leases: [],
      },
      candidateShas: { [deterministic]: branchSha },
      deliveries: {
        [deterministic]: [{
          number: 7,
          head_branch: deterministic,
          head_sha: branchSha,
          state: "closed",
          merged_at: "2026-08-28T17:30:00.000Z",
        }],
      },
    });
    const results = await runSentinelRecoveryPass({
      token: "token",
      repository: "ubiquity/ai.ubq.fi",
      owner: "run-1",
      now: "2026-08-28T18:00:00.000Z",
      fetcher,
    });
    assert.deepEqual(results.map((result) => result.action), ["resume_review"]);
    const after = ledger();
    const reconciled = after.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key)!;
    // The merged delivery moves the recovered checkpoint into review; the
    // preserved failure class must not demote it back to retry_wait.
    assert.equal(reconciled.phase, "review_pending");
    assert.equal(reconciled.disposition, "active");
    assert.equal(reconciled.failure_class, "runner_interruption");
    assert.equal(reconciled.failure_fingerprint, "8".repeat(64));
    assert.equal(after.retry_history.length, 0);
    assert.equal(after.retry_decisions.length, 0);
  },
});
