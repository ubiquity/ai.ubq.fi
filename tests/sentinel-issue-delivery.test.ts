import assert from "node:assert/strict";
import {
  createGitHubIssueJob,
  type GitHubIssueJobSource,
  parseGitHubIssueJobLedger,
  parseGitHubIssueTimeLabel,
  renderGitHubIssueJobLedger,
  selectNextGitHubIssueJob,
} from "../scripts/sentinel/issues.ts";
import {
  emptySentinelRecoveryLedger,
  type SentinelRecoveryEligibilityContext,
} from "../scripts/sentinel/recovery-ledger.ts";
import {
  evaluateIssueCompletionAction,
  isContainedDevelopmentComparison,
  isIssueDeliveryFailSafeRevert,
  isPullRequestMergeRefusalStatus,
  isSentinelRecoveryCandidateBranch,
  ISSUE_COMPLETION_EVIDENCE_TEXT,
  issuePullRequestMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseGitPushUpdates,
  parseSentinelCycleReport,
  parseSentinelRetryPendingCycleReport,
  renderIssueDeliveryEvidence,
  renderIssuePullRequestBody,
  selectDevelopmentPush,
  sentinelRecoveryCandidateBranch,
  sentinelRecoveryIdentityKey,
} from "../scripts/sentinel/issue-delivery.ts";
import {
  closeIssueAfterCompletionEvidenceRevalidation,
  compareAndSwapSentinelRecoveryRecord,
  completionEvidenceSnapshotMatches,
  mergeDeliveryPullRequest,
  readGitHubCommitRefSha,
  reconcileSentinelRecoveryRecord,
  requireCurrentOpenManualIssueSnapshot,
  validateManualRequiredRetainedCheckpointReconciliation,
  validateNativeReviewExhaustedManualCheckpointReconciliation,
  validateRetryPendingIssueReconciliation,
} from "../scripts/sentinel/issue-delivery-reconcile.ts";
import type { GitHubIssue, GitHubIssueComment } from "../scripts/sentinel/github.ts";

const emptyRecoveryContext = (): SentinelRecoveryEligibilityContext => ({
  repository: "ubiquity/ai.ubq.fi",
  ledger: emptySentinelRecoveryLedger(),
  now: "2026-08-28T06:00:00.000Z",
});

const inertIssueComments = (count: number): readonly GitHubIssueComment[] =>
  Array.from({ length: count }, (_, index) => ({
    id: 50_000 + index,
    authorLogin: "ubiquity-os[bot]",
    authorType: "Bot",
    body: `> [!WARNING]
> You are not allowed to set labels.

<!-- UbiquityOS - updateLabels - ${
      "a".repeat(64)
    } - @0x4007 - https://console.deno.com/ubiquity-os/daemon-pricing/observability/logs?start=2026-08-26T23%3A32%3A29Z&end=2026-08-26T23%3A34%3A29Z&tz=Etc%2FUTC
{
  "caller": "updateLabels"
}
-->
`,
    createdAt: "2026-08-26T23:33:29Z",
    updatedAt: "2026-08-26T23:33:29Z",
  }));

const completionEvidenceComment: GitHubIssueComment = {
  id: 90_001,
  authorLogin: "github-actions[bot]",
  authorType: "Bot",
  body: "<!-- provider-sentinel:issue-evidence:v1 -->",
  createdAt: "2026-08-25T00:01:00Z",
  updatedAt: "2026-08-25T00:01:00Z",
};

const completionEvidenceIdentity = {
  id: completionEvidenceComment.id,
  updatedAt: completionEvidenceComment.updatedAt,
};

const selection = parseGitHubIssueSelectionReport({
  schema_version: 1,
  issue_id: 5228586364,
  issue_number: 112,
  fingerprint: "a".repeat(64),
  body_sha256: "b".repeat(64),
  comments: 0,
  priority: "P3",
  time_label: "Time: <1 Day",
  files: ["src/admin.ts", "tests/admin-auth.test.ts"],
  updated_at: "2026-08-23T19:07:26Z",
});

const cycle = parseSentinelCycleReport({
  schema_version: 1,
  run_id: "123456789",
  candidate_sha: "c".repeat(40),
  temporary_branch: "sentinel/candidate-123456789",
  status: "validated",
  stage: "pushing_development",
  evidence_artifact_name: "sentinel-evidence-v1-123456789",
});

const pullRequest = parseGitHubIssuePullRequestRecord({
  schema_version: 1,
  issue_number: 112,
  fingerprint: "a".repeat(64),
  pull_request_number: 129,
  pull_request_url: "https://github.com/ubiquity/ai.ubq.fi/pull/129",
  head_branch: "sentinel/candidate-123456789",
  head_sha: "c".repeat(40),
  base_branch: "development",
  marker: issuePullRequestMarker(selection),
  reused: false,
});

const retryPendingDisposition = {
  schema_version: 1,
  issue_id: selection.issue_id,
  issue_number: selection.issue_number,
  fingerprint: selection.fingerprint,
  phase: "failed_implementation",
  implementation_status: "blocked",
  disposition: "retry_pending",
  retry_checkpoint: null,
};

const retryPendingCycle = {
  schema_version: 1,
  run_id: "123456789",
  started_at: "2026-08-27T23:00:00Z",
  base_development_sha: "d".repeat(40),
  candidate_sha: "e".repeat(40),
  temporary_branch: "sentinel/candidate-123456789-2",
  status: "no_change",
  stage: "complete",
  branch_disposition: "development_docs_only_issue_retry_pending",
  retry_checkpoint: null,
};

const retryPendingLedger = renderGitHubIssueJobLedger([{
  issueId: selection.issue_id,
  nodeId: "I_kwDOQoe6nc8AAAABN6XlfA",
  number: selection.issue_number,
  fingerprint: selection.fingerprint,
  bodySha256: selection.body_sha256,
  comments: selection.comments,
  sourceUpdatedAt: selection.updated_at,
  recordedAt: "2026-08-27T23:20:00Z",
  baseSha: retryPendingCycle.base_development_sha,
  checkpoint: null,
  title: "Retry the bounded issue",
  disposition: "retry_pending",
}]);

const recoveryIdentity = {
  repository: "ubiquity/ai.ubq.fi",
  source_kind: "github_issue" as const,
  source_id: "136",
  source_revision: "a".repeat(64),
  candidate_generation: 1,
};

const recoveryBranch = sentinelRecoveryCandidateBranch(recoveryIdentity);
const recoveryCandidateSha = "c".repeat(40);

const recoveryRecord = (overrides: Record<string, unknown> = {}) => ({
  schema_version: 1,
  identity: recoveryIdentity,
  run_id: "33197180235",
  attempt: 1,
  lease_token: "lease-1",
  base_sha: "d".repeat(40),
  phase: "recovery_pending",
  disposition: "active",
  state_version: 1,
  created_at: "2026-08-28T18:00:00.000Z",
  updated_at: "2026-08-28T18:01:00.000Z",
  candidate_branch: recoveryBranch,
  candidate_sha: recoveryCandidateSha,
  changed_files: ["src/admin.ts"],
  tree_sha: null,
  failure_class: "git_publication_ambiguity",
  failure_fingerprint: null,
  artifact_ids: [],
  artifact_digests: [],
  reason: "Candidate publication is ambiguous.",
  next_action: "Reconcile the candidate branch.",
  predecessor: null,
  ...overrides,
});

const recoveryRemote = (overrides: Record<string, unknown> = {}) => ({
  candidate_branch: recoveryBranch,
  candidate_sha: recoveryCandidateSha,
  development_sha: "e".repeat(40),
  deliveries: [],
  ...overrides,
});

const manualSnapshotIssue: GitHubIssue = {
  id: 5228586365,
  nodeId: "I_kwDOQoe6nc8AAAABN6XlfB",
  number: 113,
  state: "open" as const,
  title: "Bounded manual checkpoint issue",
  body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/admin.ts\n",
  htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/113",
  authorLogin: "0x4007",
  authorAssociation: "MEMBER",
  labels: ["Priority: 2 (Medium)", "Time: <1 Day"],
  assignees: [],
  locked: false,
  comments: 0,
  createdAt: "2026-08-28T00:00:00Z",
  updatedAt: "2026-08-28T00:00:01Z",
  isPullRequest: false,
};

const manualSnapshotRelations = {
  parentIssueNumber: null,
  subIssueCount: 0,
  blockedByCount: 0,
  blockingCount: 0,
  latestBodyEdit: null,
  latestTitleEdit: null,
};

const manualSnapshotSelection = async () => {
  const job = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    manualSnapshotIssue,
    manualSnapshotRelations,
    "admin",
    1_440,
  );
  assert.ok(job);
  return parseGitHubIssueSelectionReport({
    schema_version: 1,
    issue_id: job.issueId,
    issue_number: job.number,
    fingerprint: job.fingerprint,
    body_sha256: job.bodySha256,
    comments: job.comments,
    priority: job.priority,
    time_label: job.timeLabel,
    files: job.files,
    updated_at: job.updatedAt,
  });
};

const manualSnapshotFetcher = (
  issue: GitHubIssue,
): typeof fetch =>
(input) => {
  const url = new URL(input instanceof Request ? input.url : String(input));
  if (url.pathname === `/repos/ubiquity/ai.ubq.fi/issues/${issue.number}`) {
    return Promise.resolve(Response.json({
      id: issue.id,
      node_id: issue.nodeId,
      number: issue.number,
      state: issue.state,
      title: issue.title,
      body: issue.body,
      html_url: issue.htmlUrl,
      user: { login: issue.authorLogin },
      author_association: issue.authorAssociation,
      labels: issue.labels.map((name) => ({ name })),
      assignees: issue.assignees,
      locked: issue.locked,
      comments: issue.comments,
      created_at: issue.createdAt,
      updated_at: issue.updatedAt,
    }));
  }
  if (url.pathname === `/repos/ubiquity/ai.ubq.fi/issues/${issue.number}/sub_issues`) {
    return Promise.resolve(Response.json([]));
  }
  if (url.pathname === "/graphql") {
    return Promise.resolve(Response.json({
      data: {
        repository: {
          issue: {
            editor: null,
            lastEditedAt: null,
            timelineItems: { totalCount: 0, nodes: [] },
            parent: null,
            blockedBy: { totalCount: 0 },
            blocking: { totalCount: 0 },
          },
        },
      },
    }));
  }
  if (url.pathname === `/repos/ubiquity/ai.ubq.fi/collaborators/${issue.authorLogin}/permission`) {
    return Promise.resolve(Response.json({ permission: "admin" }));
  }
  return Promise.reject(new Error(`Unexpected GitHub request: ${url}`));
};

Deno.test("GitHub issue time labels accept deterministic estimates through one day", () => {
  assert.equal(parseGitHubIssueTimeLabel("Time: <15 Minutes"), 15);
  assert.equal(parseGitHubIssueTimeLabel("Time: <1 Hour"), 60);
  assert.equal(parseGitHubIssueTimeLabel("Time: <4 Hours"), 240);
  assert.equal(parseGitHubIssueTimeLabel("Time: <24 Hours"), 1_440);
  assert.equal(parseGitHubIssueTimeLabel("Time: <1 Day"), 1_440);
  assert.equal(parseGitHubIssueTimeLabel("Time: <25 Hours"), null);
  assert.equal(parseGitHubIssueTimeLabel("Time: <2 Days"), null);
  assert.equal(parseGitHubIssueTimeLabel("Time: <1 Hours"), null);
  assert.equal(parseGitHubIssueTimeLabel("Time: 1 Day"), null);
});

Deno.test("the production issue selector accepts a canonical estimate through one day", async () => {
  const issue = {
    id: 1,
    nodeId: "I_kwDOQoe6nc8AAAABN6Test",
    number: 1,
    state: "open" as const,
    title: "Bounded one-day issue",
    body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/http.ts\n",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/1",
    authorLogin: "0x4007",
    authorAssociation: "MEMBER",
    labels: ["Priority: 2 (Medium)", "Time: <1 Day"],
    assignees: [],
    locked: false,
    comments: 0,
    createdAt: "2026-08-25T00:00:00Z",
    updatedAt: "2026-08-25T00:00:01Z",
    isPullRequest: false,
  };
  const relations = {
    parentIssueNumber: null,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: null,
    latestTitleEdit: null,
  };
  const source = {
    listOpenIssues: () => Promise.resolve([issue]),
    getIssue: () => Promise.resolve(issue),
    listIssueComments: () => Promise.resolve([]),
    getIssueRelations: () => Promise.resolve(relations),
    getRepositoryPermission: () => Promise.resolve("write" as const),
  };
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    emptyRecoveryContext(),
  );
  assert.equal(selected?.timeLabel, "Time: <1 Day");
});

Deno.test("both manual reconciliation paths require the original open issue snapshot", async () => {
  const selected = await manualSnapshotSelection();
  const matchingInput = {
    token: "test-token",
    repository: "ubiquity/ai.ubq.fi",
    selection: selected,
  };

  await requireCurrentOpenManualIssueSnapshot({
    ...matchingInput,
    fetcher: manualSnapshotFetcher(manualSnapshotIssue),
  });

  for (
    const changedIssue of [
      {
        ...manualSnapshotIssue,
        state: "closed" as const,
        updatedAt: "2026-08-28T00:01:00Z",
      },
      {
        ...manualSnapshotIssue,
        title: "Changed bounded manual checkpoint issue",
        updatedAt: "2026-08-28T00:01:00Z",
      },
    ]
  ) {
    await assert.rejects(
      () =>
        requireCurrentOpenManualIssueSnapshot({
          ...matchingInput,
          fetcher: manualSnapshotFetcher(changedIssue),
        }),
      /issue snapshot changed or is no longer open/u,
    );
  }
});

Deno.test("retry-pending reconciliation validates a durable no-delivery receipt", () => {
  const baseInput: Parameters<typeof validateRetryPendingIssueReconciliation>[0] = {
    workflowRunId: "123456789",
    workflowFailed: false,
    selection,
    cycleValue: retryPendingCycle,
    dispositionValue: retryPendingDisposition,
    pullRequestReportPresent: false,
    productionOutcomeReportPresent: false,
    developmentLedgerMarkdown: retryPendingLedger,
  };
  assert.doesNotThrow(() => validateRetryPendingIssueReconciliation(baseInput));

  const checkpoint = {
    branch: retryPendingCycle.temporary_branch,
    sha: "1".repeat(40),
    base_sha: retryPendingCycle.base_development_sha,
  };
  const checkpointLedger = renderGitHubIssueJobLedger([{
    ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
    checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: checkpoint.base_sha },
  }]);
  const checkpointInput: Parameters<typeof validateRetryPendingIssueReconciliation>[0] = {
    ...baseInput,
    cycleValue: {
      ...retryPendingCycle,
      branch_disposition: "remote_retained_issue_retry_pending",
      retry_checkpoint: checkpoint,
    },
    dispositionValue: { ...retryPendingDisposition, retry_checkpoint: checkpoint },
    developmentLedgerMarkdown: checkpointLedger,
  };
  assert.doesNotThrow(() => validateRetryPendingIssueReconciliation(checkpointInput));

  const priorRunCheckpoint = {
    branch: "sentinel/candidate-123456700-1",
    sha: "4".repeat(40),
    base_sha: "c".repeat(40),
  };
  assert.doesNotThrow(() =>
    validateRetryPendingIssueReconciliation({
      ...baseInput,
      cycleValue: {
        ...retryPendingCycle,
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: priorRunCheckpoint,
      },
      dispositionValue: {
        ...retryPendingDisposition,
        phase: "retry_checkpoint_resume_transient",
        retry_checkpoint: priorRunCheckpoint,
      },
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
        baseSha: priorRunCheckpoint.base_sha,
        checkpoint: {
          branch: priorRunCheckpoint.branch,
          sha: priorRunCheckpoint.sha,
          baseSha: priorRunCheckpoint.base_sha,
        },
      }]),
    })
  );
  assert.throws(
    () =>
      validateRetryPendingIssueReconciliation({
        ...baseInput,
        cycleValue: {
          ...retryPendingCycle,
          branch_disposition: "remote_retained_issue_retry_pending",
          retry_checkpoint: priorRunCheckpoint,
        },
        dispositionValue: { ...retryPendingDisposition, retry_checkpoint: priorRunCheckpoint },
        developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
          ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
          baseSha: priorRunCheckpoint.base_sha,
          checkpoint: {
            branch: priorRunCheckpoint.branch,
            sha: priorRunCheckpoint.sha,
            baseSha: priorRunCheckpoint.base_sha,
          },
        }]),
      }),
    /current attempt/,
  );
  assert.throws(
    () =>
      validateRetryPendingIssueReconciliation({
        ...baseInput,
        dispositionValue: { ...retryPendingDisposition, phase: "retry_checkpoint_resume_transient" },
      }),
    /prior attempt/,
  );
  assert.throws(
    () =>
      validateRetryPendingIssueReconciliation({
        ...checkpointInput,
        dispositionValue: {
          ...retryPendingDisposition,
          phase: "retry_checkpoint_resume_transient",
          retry_checkpoint: checkpoint,
        },
      }),
    /prior attempt/,
  );

  const checkpointMismatchInputs: Array<
    readonly [Partial<Parameters<typeof validateRetryPendingIssueReconciliation>[0]>, RegExp]
  > = [
    [{
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(checkpointLedger)[0]!,
        checkpoint: { branch: "sentinel/candidate-123456780", sha: checkpoint.sha, baseSha: checkpoint.base_sha },
      }]),
    }, /exact durable ledger row/],
    [{
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(checkpointLedger)[0]!,
        checkpoint: { branch: checkpoint.branch, sha: "2".repeat(40), baseSha: checkpoint.base_sha },
      }]),
    }, /exact durable ledger row/],
    [{
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(checkpointLedger)[0]!,
        baseSha: "3".repeat(40),
        checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: "3".repeat(40) },
      }]),
    }, /exact durable ledger row/],
    [{ dispositionValue: retryPendingDisposition }, /exact durable ledger row/],
    [{ cycleValue: retryPendingCycle }, /exact durable ledger row/],
    [{ developmentLedgerMarkdown: retryPendingLedger }, /exact durable ledger row/],
    [{
      cycleValue: {
        ...retryPendingCycle,
        temporary_branch: "sentinel/candidate-123456780",
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, branch: "sentinel/candidate-123456780" },
      },
    }, /exact durable ledger row/],
    [{
      cycleValue: {
        ...retryPendingCycle,
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, sha: "2".repeat(40) },
      },
    }, /exact durable ledger row/],
    [{
      cycleValue: {
        ...retryPendingCycle,
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, base_sha: "3".repeat(40) },
      },
    }, /exact durable ledger row/],
    [{
      cycleValue: {
        ...retryPendingCycle,
        branch_disposition: "development_docs_only_issue_retry_pending",
        retry_checkpoint: checkpoint,
      },
    }, /checkpoint does not match its branch disposition/],
  ];
  for (const [overrides, pattern] of checkpointMismatchInputs) {
    assert.throws(
      () => validateRetryPendingIssueReconciliation({ ...checkpointInput, ...overrides }),
      pattern,
    );
  }

  const invalidInputs: Array<
    readonly [Partial<Parameters<typeof validateRetryPendingIssueReconciliation>[0]>, RegExp]
  > = [
    [{ workflowFailed: true }, /cycle report is invalid/],
    [{ pullRequestReportPresent: true }, /cannot contain delivery or production records/],
    [{ productionOutcomeReportPresent: true }, /cannot contain delivery or production records/],
    [{ dispositionValue: { ...retryPendingDisposition, issue_id: 999 } }, /exact issue selection/],
    [{ dispositionValue: { ...retryPendingDisposition, phase: "initial_implementation" } }, /exact issue selection/],
    [{ cycleValue: { ...retryPendingCycle, run_id: "987654321" } }, /cycle report is invalid/],
    [{ cycleValue: { ...retryPendingCycle, stage: "pushing_retry_pending_github_issue" } }, /cycle report is invalid/],
    [{ developmentLedgerMarkdown: renderGitHubIssueJobLedger([]) }, /exact durable ledger row/],
    [{
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
        baseSha: "c".repeat(40),
      }]),
    }, /exact durable ledger row/],
    [{
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
        disposition: "resolved",
      }]),
    }, /exact durable ledger row/],
  ];
  for (const [overrides, pattern] of invalidInputs) {
    assert.throws(() => validateRetryPendingIssueReconciliation({ ...baseInput, ...overrides }), pattern);
  }
});

Deno.test("failed atomic retry reconciliation requires exact durable remote refs", () => {
  const checkpoint = {
    branch: retryPendingCycle.temporary_branch,
    sha: "1".repeat(40),
    base_sha: retryPendingCycle.base_development_sha,
  };
  const cycleValue = {
    ...retryPendingCycle,
    status: "failed",
    stage: "failed",
    branch_disposition: "atomic_retry_push_requires_reconciliation",
    retry_checkpoint: checkpoint,
  };
  const developmentLedgerMarkdown = renderGitHubIssueJobLedger([{
    ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
    checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: checkpoint.base_sha },
  }]);
  const input: Parameters<typeof validateRetryPendingIssueReconciliation>[0] = {
    workflowRunId: "123456789",
    workflowFailed: true,
    selection,
    cycleValue,
    dispositionValue: { ...retryPendingDisposition, retry_checkpoint: checkpoint },
    pullRequestReportPresent: false,
    productionOutcomeReportPresent: false,
    developmentLedgerMarkdown,
    remoteRefs: {
      developmentSha: retryPendingCycle.candidate_sha,
      checkpointSha: checkpoint.sha,
    },
  };
  assert.doesNotThrow(() => validateRetryPendingIssueReconciliation(input));
  assert.throws(
    () =>
      validateRetryPendingIssueReconciliation({
        ...input,
        remoteRefs: { ...input.remoteRefs!, developmentSha: "2".repeat(40) },
      }),
    /development ref does not match the candidate commit/,
  );
  assert.throws(
    () =>
      validateRetryPendingIssueReconciliation({
        ...input,
        remoteRefs: { ...input.remoteRefs!, checkpointSha: "3".repeat(40) },
      }),
    /checkpoint ref does not match the durable checkpoint/,
  );
  const { remoteRefs: _remoteRefs, ...withoutRemoteRefs } = input;
  assert.throws(
    () => validateRetryPendingIssueReconciliation(withoutRemoteRefs),
    /requires exact durable remote refs/,
  );

  const resumedCheckpoint = {
    branch: "sentinel/candidate-123456700-1",
    sha: "4".repeat(40),
    base_sha: "c".repeat(40),
  };
  assert.doesNotThrow(() =>
    validateRetryPendingIssueReconciliation({
      ...input,
      cycleValue: {
        ...cycleValue,
        temporary_branch: "sentinel/candidate-123456789-3",
        retry_checkpoint: resumedCheckpoint,
      },
      dispositionValue: {
        ...retryPendingDisposition,
        phase: "retry_checkpoint_resume_transient",
        retry_checkpoint: resumedCheckpoint,
      },
      developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
        ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
        baseSha: resumedCheckpoint.base_sha,
        checkpoint: {
          branch: resumedCheckpoint.branch,
          sha: resumedCheckpoint.sha,
          baseSha: resumedCheckpoint.base_sha,
        },
      }]),
      remoteRefs: {
        developmentSha: retryPendingCycle.candidate_sha,
        checkpointSha: resumedCheckpoint.sha,
      },
    })
  );
});

Deno.test("native review exhaustion reconciliation requires the exact atomic manual receipt", () => {
  const checkpoint = {
    branch: "sentinel/candidate-123456789-2",
    sha: "f".repeat(40),
    base_sha: "d".repeat(40),
  };
  const disposition = {
    schema_version: 1,
    issue_id: selection.issue_id,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    phase: "native_review_exhausted",
    implementation_status: "blocked",
    disposition: "manual_required",
    retry_checkpoint: checkpoint,
  };
  const cycle = {
    schema_version: 1,
    run_id: "123456789",
    started_at: "2026-08-28T00:00:00Z",
    base_development_sha: checkpoint.base_sha,
    candidate_sha: "e".repeat(40),
    temporary_branch: checkpoint.branch,
    status: "no_change",
    stage: "complete",
    branch_disposition: "remote_retained_issue_manual_required",
    retry_checkpoint: checkpoint,
  };
  const manualLedger = (recordedAt = "2026-08-28T00:01:00Z") =>
    renderGitHubIssueJobLedger([{
      ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
      recordedAt,
      baseSha: checkpoint.base_sha,
      checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: checkpoint.base_sha },
      disposition: "manual_required",
    }]);
  const input: Parameters<typeof validateNativeReviewExhaustedManualCheckpointReconciliation>[0] = {
    workflowRunId: cycle.run_id,
    workflowRunAttempt: 2,
    workflowFailed: false,
    selection,
    cycleValue: cycle,
    dispositionValue: disposition,
    pullRequestReportPresent: false,
    productionOutcomeReportPresent: false,
    developmentLedgerMarkdown: manualLedger(),
    remoteRefs: {
      developmentSha: cycle.candidate_sha,
      checkpointSha: checkpoint.sha,
    },
  };

  assert.doesNotThrow(() => validateNativeReviewExhaustedManualCheckpointReconciliation(input));
  assert.doesNotThrow(() =>
    validateNativeReviewExhaustedManualCheckpointReconciliation({
      ...input,
      workflowFailed: true,
      cycleValue: {
        ...cycle,
        status: "failed",
        stage: "failed",
        branch_disposition: "atomic_manual_push_requires_reconciliation",
      },
    })
  );

  for (
    const [overrides, pattern] of [
      [{ remoteRefs: { ...input.remoteRefs, developmentSha: "1".repeat(40) } }, /exact durable remote refs/u],
      [{ remoteRefs: { ...input.remoteRefs, checkpointSha: "1".repeat(40) } }, /exact durable remote refs/u],
      [{ remoteRefs: { developmentSha: "", checkpointSha: "" } }, /exact durable remote refs/u],
      [{ developmentLedgerMarkdown: manualLedger("2026-08-27T23:59:59Z") }, /exact durable ledger row/u],
      [{ pullRequestReportPresent: true }, /cannot contain delivery or production records/u],
      [{ productionOutcomeReportPresent: true }, /cannot contain delivery or production records/u],
      [{ workflowRunAttempt: 3 }, /manual-checkpoint cycle report is invalid/u],
      [{
        cycleValue: {
          ...cycle,
          temporary_branch: "sentinel/candidate-123456789-3",
          retry_checkpoint: { ...checkpoint, branch: "sentinel/candidate-123456789-3" },
        },
      }, /manual-checkpoint cycle report is invalid/u],
      [{ workflowFailed: true }, /manual-checkpoint cycle report is invalid/u],
    ] as const
  ) {
    assert.throws(
      () => validateNativeReviewExhaustedManualCheckpointReconciliation({ ...input, ...overrides }),
      pattern,
    );
  }
});

Deno.test("retained checkpoint manual reconciliation stays docs-only and exact", () => {
  const checkpoint = {
    branch: "sentinel/candidate-123456700-1",
    sha: "f".repeat(40),
    base_sha: "c".repeat(40),
  };
  const disposition = {
    schema_version: 1,
    issue_id: selection.issue_id,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    phase: "retry_checkpoint_resume_failed",
    implementation_status: "blocked",
    disposition: "manual_required",
    retry_checkpoint: checkpoint,
  };
  const cycle = {
    schema_version: 1,
    run_id: "123456789",
    started_at: "2026-08-28T00:00:00Z",
    base_development_sha: "d".repeat(40),
    candidate_sha: "e".repeat(40),
    temporary_branch: "sentinel/candidate-123456789-2",
    status: "no_change",
    stage: "complete",
    branch_disposition: "development_docs_only_issue_manual_required",
    retry_checkpoint: checkpoint,
  };
  const input: Parameters<typeof validateManualRequiredRetainedCheckpointReconciliation>[0] = {
    workflowRunId: cycle.run_id,
    selection,
    cycleValue: cycle,
    dispositionValue: disposition,
    pullRequestReportPresent: false,
    productionOutcomeReportPresent: false,
    developmentLedgerMarkdown: renderGitHubIssueJobLedger([{
      ...parseGitHubIssueJobLedger(retryPendingLedger)[0]!,
      recordedAt: "2026-08-28T00:01:00Z",
      baseSha: checkpoint.base_sha,
      checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: checkpoint.base_sha },
      disposition: "manual_required",
    }]),
    remoteRefs: {
      developmentSha: cycle.candidate_sha,
      checkpointSha: checkpoint.sha,
    },
  };

  assert.doesNotThrow(() => validateManualRequiredRetainedCheckpointReconciliation(input));
  assert.throws(
    () => validateManualRequiredRetainedCheckpointReconciliation({ ...input, pullRequestReportPresent: true }),
    /cannot contain delivery or production records/u,
  );
  assert.throws(
    () =>
      validateManualRequiredRetainedCheckpointReconciliation({
        ...input,
        remoteRefs: { ...input.remoteRefs, checkpointSha: "1".repeat(40) },
      }),
    /exact durable remote refs/u,
  );
});

Deno.test("GitHub atomic retry ref reads require exact commit identities", async () => {
  const developmentSha = "d".repeat(40);
  const checkpointBranch = "sentinel/candidate-123456789-2";
  const checkpointSha = "e".repeat(40);
  const requests: string[] = [];
  const validFetcher: typeof fetch = (input) => {
    const url = input instanceof Request ? input.url : String(input);
    requests.push(new URL(url).pathname);
    if (url.endsWith("/git/ref/heads/development")) {
      return Promise.resolve(Response.json({
        ref: "refs/heads/development",
        object: { type: "commit", sha: developmentSha },
      }));
    }
    if (url.endsWith(`/git/ref/heads/${checkpointBranch}`)) {
      return Promise.resolve(Response.json({
        ref: `refs/heads/${checkpointBranch}`,
        object: { type: "commit", sha: checkpointSha },
      }));
    }
    return Promise.reject(new Error(`Unexpected GitHub request: ${url}`));
  };
  assert.equal(
    await readGitHubCommitRefSha("test-token", "ubiquity/ai.ubq.fi", "development", validFetcher),
    developmentSha,
  );
  assert.equal(
    await readGitHubCommitRefSha("test-token", "ubiquity/ai.ubq.fi", checkpointBranch, validFetcher),
    checkpointSha,
  );
  assert.deepEqual(requests, [
    "/repos/ubiquity/ai.ubq.fi/git/ref/heads/development",
    `/repos/ubiquity/ai.ubq.fi/git/ref/heads/${checkpointBranch}`,
  ]);

  for (
    const payload of [
      { ref: "refs/heads/other", object: { type: "commit", sha: developmentSha } },
      { ref: "refs/heads/development", object: { type: "tree", sha: developmentSha } },
      { ref: "refs/heads/development", object: { type: "commit", sha: "a".repeat(39) } },
    ]
  ) {
    const malformedFetcher: typeof fetch = () => Promise.resolve(Response.json(payload));
    await assert.rejects(
      () => readGitHubCommitRefSha("test-token", "ubiquity/ai.ubq.fi", "development", malformedFetcher),
      /invalid commit identity/,
    );
  }

  const missingFetcher: typeof fetch = () => Promise.resolve(Response.json({}, { status: 404 }));
  await assert.rejects(
    () => readGitHubCommitRefSha("test-token", "ubiquity/ai.ubq.fi", "development", missingFetcher),
    /HTTP 404/,
  );
  await assert.rejects(
    () => readGitHubCommitRefSha("test-token", "ubiquity/ai.ubq.fi", checkpointBranch, missingFetcher),
    /HTTP 404/,
  );
});

Deno.test("durable completion evidence never closes a changed issue snapshot", async () => {
  const originalIssue = {
    id: 2,
    nodeId: "I_kwDOQoe6nc8AAAABN7Test",
    number: 2,
    state: "open" as const,
    title: "Bounded durable issue",
    body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/http.ts\n",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/2",
    authorLogin: "0x4007",
    authorAssociation: "MEMBER",
    labels: ["Priority: 2 (Medium)", "Time: <1 Day"],
    assignees: [],
    locked: false,
    comments: 3,
    createdAt: "2026-08-25T00:00:00Z",
    updatedAt: "2026-08-25T00:00:01Z",
    isPullRequest: false,
  };
  const relations = {
    parentIssueNumber: null,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: null,
    latestTitleEdit: null,
  };
  let comments = inertIssueComments(originalIssue.comments);
  const source = {
    listOpenIssues: () => Promise.resolve([originalIssue]),
    getIssue: () => Promise.resolve(originalIssue),
    listIssueComments: () => Promise.resolve(comments),
    getIssueRelations: () => Promise.resolve(relations),
    getRepositoryPermission: () => Promise.resolve("write" as const),
  };
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
    emptyRecoveryContext(),
  );
  assert.ok(selected);
  const retrySelection = parseGitHubIssueSelectionReport({
    schema_version: 1,
    issue_id: selected.issueId,
    issue_number: selected.number,
    fingerprint: selected.fingerprint,
    body_sha256: selected.bodySha256,
    comments: selected.comments,
    priority: selected.priority,
    time_label: selected.timeLabel,
    files: selected.files,
    updated_at: selected.updatedAt,
  });
  comments = [...comments, completionEvidenceComment];
  const evidenceOnlyIssue = { ...originalIssue, comments: 4, updatedAt: "2026-08-25T00:01:01Z" };
  assert.equal(
    await completionEvidenceSnapshotMatches(
      source,
      "ubiquity/ai.ubq.fi",
      retrySelection,
      evidenceOnlyIssue,
      completionEvidenceIdentity,
    ),
    true,
  );
  assert.equal(
    await completionEvidenceSnapshotMatches(
      source,
      "ubiquity/ai.ubq.fi",
      retrySelection,
      { ...evidenceOnlyIssue, body: `${evidenceOnlyIssue.body}\nChanged after completion evidence.\n` },
      completionEvidenceIdentity,
    ),
    false,
  );
  assert.equal(
    await completionEvidenceSnapshotMatches(
      source,
      "ubiquity/ai.ubq.fi",
      retrySelection,
      { ...evidenceOnlyIssue, comments: 5 },
      completionEvidenceIdentity,
    ),
    false,
  );
});

Deno.test("normal delivery revalidates the snapshot after completion evidence before closing", async () => {
  const originalIssue = {
    id: 2,
    nodeId: "I_kwDOQoe6nc8AAAABN7Test",
    number: 2,
    state: "open" as const,
    title: "Bounded durable issue",
    body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/http.ts\n",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/2",
    authorLogin: "0x4007",
    authorAssociation: "MEMBER",
    labels: ["Priority: 2 (Medium)", "Time: <1 Day"],
    assignees: [],
    locked: false,
    comments: 3,
    createdAt: "2026-08-25T00:00:00Z",
    updatedAt: "2026-08-25T00:00:01Z",
    isPullRequest: false,
  };
  const relations = {
    parentIssueNumber: null,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: null,
    latestTitleEdit: null,
  };
  const selected = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    originalIssue,
    relations,
    "admin",
    1_440,
  );
  assert.ok(selected);
  const selectionReport = parseGitHubIssueSelectionReport({
    schema_version: 1,
    issue_id: selected.issueId,
    issue_number: selected.number,
    fingerprint: selected.fingerprint,
    body_sha256: selected.bodySha256,
    comments: selected.comments,
    priority: selected.priority,
    time_label: selected.timeLabel,
    files: selected.files,
    updated_at: selected.updatedAt,
  });
  const changedAfterEvidence = {
    ...originalIssue,
    body: `${originalIssue.body}\nChanged after the initial snapshot check.\n`,
    comments: 5,
    updatedAt: "2026-08-25T00:01:01Z",
  };
  const source: GitHubIssueJobSource = {
    listOpenIssues: () => Promise.resolve([changedAfterEvidence]),
    getIssue: () => Promise.resolve(changedAfterEvidence),
    listIssueComments: () =>
      Promise.resolve([
        ...inertIssueComments(originalIssue.comments),
        completionEvidenceComment,
        {
          id: 90_002,
          authorLogin: "human-reviewer",
          authorType: "User",
          body: "Changed after completion evidence.",
          createdAt: "2026-08-25T00:01:01Z",
          updatedAt: "2026-08-25T00:01:01Z",
        },
      ]),
    getIssueRelations: () => Promise.resolve(relations),
    getRepositoryPermission: () => Promise.resolve("admin"),
  };
  let closed = false;
  await assert.rejects(
    () =>
      closeIssueAfterCompletionEvidenceRevalidation(
        source,
        "ubiquity/ai.ubq.fi",
        selectionReport,
        completionEvidenceIdentity,
        () => {
          closed = true;
          return Promise.resolve();
        },
      ),
    /no longer matches the open issue snapshot/,
  );
  assert.equal(closed, false);
});

Deno.test("issue closure rejects an edit during asynchronous snapshot validation", async () => {
  const originalIssue = {
    id: 2,
    nodeId: "I_kwDOQoe6nc8AAAABN7Test",
    number: 2,
    state: "open" as const,
    title: "Bounded durable issue",
    body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/http.ts\n",
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/2",
    authorLogin: "0x4007",
    authorAssociation: "MEMBER",
    labels: ["Priority: 2 (Medium)", "Time: <1 Day"],
    assignees: [],
    locked: false,
    comments: 3,
    createdAt: "2026-08-25T00:00:00Z",
    updatedAt: "2026-08-25T00:00:01Z",
    isPullRequest: false,
  };
  const relations = {
    parentIssueNumber: null,
    subIssueCount: 0,
    blockedByCount: 0,
    blockingCount: 0,
    latestBodyEdit: null,
    latestTitleEdit: null,
  };
  const selected = await createGitHubIssueJob(
    "ubiquity/ai.ubq.fi",
    originalIssue,
    relations,
    "admin",
    1_440,
  );
  assert.ok(selected);
  const selectionReport = parseGitHubIssueSelectionReport({
    schema_version: 1,
    issue_id: selected.issueId,
    issue_number: selected.number,
    fingerprint: selected.fingerprint,
    body_sha256: selected.bodySha256,
    comments: selected.comments,
    priority: selected.priority,
    time_label: selected.timeLabel,
    files: selected.files,
    updated_at: selected.updatedAt,
  });
  let liveIssue = { ...originalIssue, comments: 4, updatedAt: "2026-08-25T00:01:01Z" };
  const source: GitHubIssueJobSource = {
    listOpenIssues: () => Promise.resolve([liveIssue]),
    getIssue: () => Promise.resolve(liveIssue),
    listIssueComments: () =>
      Promise.resolve([...inertIssueComments(originalIssue.comments), completionEvidenceComment]),
    getIssueRelations: () => {
      liveIssue = {
        ...liveIssue,
        body: `${liveIssue.body}\nHuman edit during validation.\n`,
        updatedAt: "2026-08-25T00:01:02Z",
      };
      return Promise.resolve(relations);
    },
    getRepositoryPermission: () => Promise.resolve("admin"),
  };
  let closed = false;
  await assert.rejects(
    () =>
      closeIssueAfterCompletionEvidenceRevalidation(
        source,
        "ubiquity/ai.ubq.fi",
        selectionReport,
        completionEvidenceIdentity,
        () => {
          closed = true;
          return Promise.resolve();
        },
      ),
    /no longer matches the open issue snapshot/,
  );
  assert.equal(closed, false);
});

Deno.test("GitHub issue selection reports require a safe nonnegative comment count", () => {
  const rawSelection = {
    schema_version: 1,
    issue_id: 5228586364,
    issue_number: 112,
    fingerprint: "a".repeat(64),
    body_sha256: "b".repeat(64),
    comments: 3,
    priority: "P3",
    time_label: "Time: <1 Day",
    files: ["src/admin.ts"],
    updated_at: "2026-08-23T19:07:26Z",
  };
  assert.equal(parseGitHubIssueSelectionReport(rawSelection).comments, 3);
  for (const comments of [-1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(
      () => parseGitHubIssueSelectionReport({ ...rawSelection, comments }),
      /selection report is invalid/,
    );
  }
  const { comments: _comments, ...missingComments } = rawSelection;
  assert.throws(() => parseGitHubIssueSelectionReport(missingComments), /selection report is invalid/);
});

Deno.test("pre-push parsing isolates exactly one development update", () => {
  const zero = "0".repeat(40);
  const updates = parseGitPushUpdates([
    `${"c".repeat(40)} ${"c".repeat(40)} refs/heads/sentinel/candidate-123 ${zero}`,
    `HEAD ${"c".repeat(40)} refs/heads/development ${"d".repeat(40)}`,
    "",
  ].join("\n"));
  assert.equal(updates[0]?.localRef, "c".repeat(40));
  assert.equal(selectDevelopmentPush(updates)?.localSha, "c".repeat(40));
  assert.throws(
    () => selectDevelopmentPush([...updates, updates[1]!]),
    /multiple development updates/,
  );
});

Deno.test("retry-pending cycle parsing distinguishes local checkpoint preparation from remote retention", () => {
  const checkpoint = {
    branch: "sentinel/candidate-123456789-1",
    sha: "c".repeat(40),
    base_sha: "d".repeat(40),
  };
  const cycle = {
    schema_version: 1,
    run_id: "123456789",
    started_at: "2026-08-28T08:00:00Z",
    base_development_sha: checkpoint.base_sha,
    candidate_sha: "e".repeat(40),
    temporary_branch: checkpoint.branch,
    status: "running",
    stage: "pushing_retry_pending_github_issue",
    branch_disposition: "runner_local_pending_review",
    retry_checkpoint: checkpoint,
  };
  assert.equal(
    parseSentinelRetryPendingCycleReport(cycle, {
      runId: cycle.run_id,
      status: "running",
      stage: "pushing_retry_pending_github_issue",
      branchDispositions: [
        "runner_local_pending_review",
        "runner_local_atomic_push_in_flight",
        "remote_retained_issue_retry_pending",
        "remote_retained_atomic_push_in_flight",
      ],
    }).branch_disposition,
    "runner_local_pending_review",
  );
  assert.equal(
    parseSentinelRetryPendingCycleReport({
      ...cycle,
      branch_disposition: "runner_local_atomic_push_in_flight",
    }, {
      runId: cycle.run_id,
      status: "running",
      stage: "pushing_retry_pending_github_issue",
      branchDispositions: [
        "runner_local_pending_review",
        "runner_local_atomic_push_in_flight",
        "remote_retained_issue_retry_pending",
        "remote_retained_atomic_push_in_flight",
      ],
    }).branch_disposition,
    "runner_local_atomic_push_in_flight",
  );
  assert.equal(
    parseSentinelRetryPendingCycleReport({
      ...cycle,
      branch_disposition: "remote_retained_issue_retry_pending",
    }, {
      runId: cycle.run_id,
      status: "running",
      stage: "pushing_retry_pending_github_issue",
      branchDispositions: [
        "runner_local_pending_review",
        "runner_local_atomic_push_in_flight",
        "remote_retained_issue_retry_pending",
        "remote_retained_atomic_push_in_flight",
      ],
    }).branch_disposition,
    "remote_retained_issue_retry_pending",
  );
  assert.equal(
    parseSentinelRetryPendingCycleReport({
      ...cycle,
      branch_disposition: "remote_retained_atomic_push_in_flight",
    }, {
      runId: cycle.run_id,
      status: "running",
      stage: "pushing_retry_pending_github_issue",
      branchDispositions: [
        "runner_local_pending_review",
        "runner_local_atomic_push_in_flight",
        "remote_retained_issue_retry_pending",
        "remote_retained_atomic_push_in_flight",
      ],
    }).branch_disposition,
    "remote_retained_atomic_push_in_flight",
  );
  assert.throws(
    () =>
      parseSentinelRetryPendingCycleReport({
        ...cycle,
        branch_disposition: "development_docs_only_issue_retry_pending",
      }, {
        runId: cycle.run_id,
        status: "running",
        stage: "pushing_retry_pending_github_issue",
        branchDispositions: ["development_docs_only_issue_retry_pending"],
      }),
    /checkpoint does not match its branch disposition/u,
  );
});

Deno.test("the issue PR gate permits only the exact one-parent fail-safe revert", () => {
  assert.equal(
    isIssueDeliveryFailSafeRevert({
      selection,
      cycle,
      pullRequest,
      pushedSha: "d".repeat(40),
      parentSha: "c".repeat(40),
    }),
    true,
  );
  assert.equal(
    isIssueDeliveryFailSafeRevert({
      selection,
      cycle,
      pullRequest,
      pushedSha: "d".repeat(40),
      parentSha: "e".repeat(40),
    }),
    false,
  );
});

Deno.test("the delivery pull request links the issue as evidence without a closing keyword", () => {
  const body = renderIssuePullRequestBody({
    repository: "ubiquity/ai.ubq.fi",
    selection,
    cycle,
    candidateSha: "c".repeat(40),
    workflowRunUrl: "https://github.com/ubiquity/ai.ubq.fi/actions/runs/123456789",
    validationReports: ["validation-round-1.json"],
    nativeReviewReports: ["native-review-round-1.json"],
    replayReports: ["replay-round-1.json"],
    previewRevision: "revision_123",
    previewWorkflowRunId: 987654321,
  });
  assert.equal(body.match(/provider-sentinel:issue-pr:v1/gu)?.length, 1);
  assert.match(body, /single delivery record/);
  assert.match(body, /\[#112\]\(https:\/\/github\.com\/ubiquity\/ai\.ubq\.fi\/issues\/112\)/u);
  assert.match(body, /Sentinel merges this pull request and closes the issue/u);
  assert.match(body, /validation-round-1\.json/);
  assert.doesNotMatch(body, /(?:close[sd]?|fixe[sd]?)\s+#112/iu);
});

Deno.test("merge refusals are contained only when the head is already in development", () => {
  assert.equal(isPullRequestMergeRefusalStatus(403), true);
  assert.equal(isPullRequestMergeRefusalStatus(405), true);
  assert.equal(isPullRequestMergeRefusalStatus(409), true);
  assert.equal(isPullRequestMergeRefusalStatus(422), true);
  assert.equal(isPullRequestMergeRefusalStatus(200), false);
  assert.equal(isPullRequestMergeRefusalStatus(404), false);
  assert.equal(isContainedDevelopmentComparison("behind"), true);
  assert.equal(isContainedDevelopmentComparison("identical"), true);
  assert.equal(isContainedDevelopmentComparison("ahead"), false);
  assert.equal(isContainedDevelopmentComparison("diverged"), false);
});

Deno.test("the issue delivery merge pins the immutable pull-request head SHA", async () => {
  const requests: Array<Readonly<{ url: string; method: string; body: string | null }>> = [];
  const fetcher: typeof fetch = (input, init = {}) => {
    const url = input instanceof Request ? input.url : String(input);
    const method = init.method ?? (input instanceof Request ? input.method : "GET");
    requests.push({ url, method, body: typeof init.body === "string" ? init.body : null });
    if (method === "GET" && url.endsWith("/pulls/129")) {
      return Promise.resolve(Response.json({
        number: 129,
        state: "open",
        merged_at: null,
        head: { ref: pullRequest.head_branch, sha: pullRequest.head_sha },
        base: { ref: "development" },
      }));
    }
    if (method === "PUT" && url.endsWith("/pulls/129/merge")) {
      return Promise.resolve(Response.json({ merged: true }));
    }
    return Promise.reject(new Error(`Unexpected GitHub request: ${method} ${url}`));
  };

  assert.deepEqual(
    await mergeDeliveryPullRequest("test-token", "ubiquity/ai.ubq.fi", pullRequest, fetcher),
    { source: "sentinel_merge_api" },
  );
  const mergeRequest = requests.find((request) => request.method === "PUT");
  assert.ok(mergeRequest?.body);
  assert.deepEqual(JSON.parse(mergeRequest.body), {
    merge_method: "merge",
    commit_title: "merge: Provider Sentinel deliverable for #112",
    sha: pullRequest.head_sha,
  });
});

Deno.test("the issue delivery merge rejects a changed pull-request head before merging", async () => {
  let requests = 0;
  const fetcher: typeof fetch = () => {
    requests++;
    return Promise.resolve(Response.json({
      number: 129,
      state: "open",
      merged_at: null,
      head: { ref: pullRequest.head_branch, sha: "d".repeat(40) },
      base: { ref: "development" },
    }));
  };

  await assert.rejects(
    () => mergeDeliveryPullRequest("test-token", "ubiquity/ai.ubq.fi", pullRequest, fetcher),
    /changed identity/,
  );
  assert.equal(requests, 1);
});

Deno.test("the issue delivery merge accepts a refusal only after development containment", async () => {
  const fetcher: typeof fetch = (input, init = {}) => {
    const url = input instanceof Request ? input.url : String(input);
    const method = init.method ?? (input instanceof Request ? input.method : "GET");
    if (method === "GET" && url.endsWith("/pulls/129")) {
      return Promise.resolve(Response.json({
        number: 129,
        state: "open",
        merged_at: null,
        head: { ref: pullRequest.head_branch, sha: pullRequest.head_sha },
        base: { ref: "development" },
      }));
    }
    if (method === "PUT" && url.endsWith("/pulls/129/merge")) {
      return Promise.resolve(Response.json({ merged: false }, { status: 409 }));
    }
    if (method === "GET" && url.includes("/compare/development...")) {
      return Promise.resolve(Response.json({ status: "behind" }));
    }
    return Promise.reject(new Error(`Unexpected GitHub request: ${method} ${url}`));
  };

  assert.deepEqual(
    await mergeDeliveryPullRequest("test-token", "ubiquity/ai.ubq.fi", pullRequest, fetcher),
    { source: "development_content" },
  );
  const uncontainedFetcher: typeof fetch = (input, init = {}) => {
    const url = input instanceof Request ? input.url : String(input);
    if (url.includes("/compare/development...")) {
      return Promise.resolve(Response.json({ status: "ahead" }));
    }
    return fetcher(input, init);
  };
  await assert.rejects(
    () => mergeDeliveryPullRequest("test-token", "ubiquity/ai.ubq.fi", pullRequest, uncontainedFetcher),
    /failed with HTTP 409/,
  );
});

Deno.test("the issue delivery merge rejects a changed head after a merge refusal", async () => {
  let pullRequestFetches = 0;
  let compared = false;
  const fetcher: typeof fetch = (input, init = {}) => {
    const url = input instanceof Request ? input.url : String(input);
    const method = init.method ?? (input instanceof Request ? input.method : "GET");
    if (method === "GET" && url.endsWith("/pulls/129")) {
      pullRequestFetches++;
      return Promise.resolve(Response.json({
        number: 129,
        state: "open",
        merged_at: null,
        head: {
          ref: pullRequest.head_branch,
          sha: pullRequestFetches === 1 ? pullRequest.head_sha : "d".repeat(40),
        },
        base: { ref: "development" },
      }));
    }
    if (method === "PUT" && url.endsWith("/pulls/129/merge")) {
      return Promise.resolve(Response.json({ merged: false }, { status: 409 }));
    }
    if (method === "GET" && url.includes("/compare/development...")) {
      compared = true;
      return Promise.resolve(Response.json({ status: "behind" }));
    }
    return Promise.reject(new Error(`Unexpected GitHub request: ${method} ${url}`));
  };

  await assert.rejects(
    () => mergeDeliveryPullRequest("test-token", "ubiquity/ai.ubq.fi", pullRequest, fetcher),
    /changed identity/,
  );
  assert.equal(pullRequestFetches, 2);
  assert.equal(compared, false);
});

Deno.test("issue closure requires a merged PR, a kept deployment, and the same snapshot", () => {
  const base = {
    hasSelection: true,
    workflowFailed: false,
    disposition: "resolved" as const,
    outcome: "kept" as const,
    pullRequestMerged: true,
    issueSnapshotMatches: true,
  };
  assert.equal(evaluateIssueCompletionAction(base), "close_completed");
  assert.equal(evaluateIssueCompletionAction({ ...base, pullRequestMerged: false }), "leave_open_failed");
  assert.equal(evaluateIssueCompletionAction({ ...base, issueSnapshotMatches: false }), "leave_open_failed");
  assert.equal(
    evaluateIssueCompletionAction({ ...base, outcome: "rolled_back" }),
    "leave_open_rolled_back",
  );
  assert.equal(
    evaluateIssueCompletionAction({ ...base, disposition: "manual_required", outcome: null }),
    "leave_open_manual_required",
  );
});

Deno.test("final evidence binds the issue, PR, workflow, commit, and production result", () => {
  const evidence = renderIssueDeliveryEvidence({
    repository: "ubiquity/ai.ubq.fi",
    selection,
    pullRequest,
    workflowRunUrl: "https://github.com/ubiquity/ai.ubq.fi/actions/runs/123456789",
    action: "close_completed",
    cycleStatus: "kept",
    candidateSha: "c".repeat(40),
    productionRevision: "revision_123",
    deploymentWorkflowRunId: 111,
    promotionWorkflowRunId: 222,
    monitoringDecision: "keep",
  });
  assert.match(evidence, /pull\/129/);
  assert.match(evidence, new RegExp("c".repeat(40), "u"));
  assert.ok(evidence.includes(ISSUE_COMPLETION_EVIDENCE_TEXT));
  assert.match(evidence, /closed as completed/);
  assert.match(evidence, /Monitoring decision: keep/);
});

Deno.test("recovery candidate branches are deterministic and include source generation", () => {
  assert.equal(sentinelRecoveryCandidateBranch(recoveryIdentity), recoveryBranch);
  assert.equal(sentinelRecoveryCandidateBranch(recoveryIdentity), sentinelRecoveryCandidateBranch(recoveryIdentity));
  assert.equal(isSentinelRecoveryCandidateBranch(recoveryBranch), true);
  assert.match(recoveryBranch, /github_issue-136-a{32}-g1/u);
  assert.notEqual(
    sentinelRecoveryIdentityKey(recoveryIdentity),
    sentinelRecoveryIdentityKey({ ...recoveryIdentity, candidate_generation: 2 }),
  );
});

Deno.test("recovery reconciliation confirms one remote checkpoint without changing its branch identity", () => {
  const first = reconcileSentinelRecoveryRecord({
    record: recoveryRecord(),
    remote: recoveryRemote(),
    now: "2026-08-28T18:02:00.000Z",
    expected_state_version: 1,
    expected_lease_token: "lease-1",
  });
  assert.equal(first.action, "checkpoint_confirmed");
  assert.equal(first.after.phase, "checkpoint_durable");
  assert.equal(first.after.disposition, "active");
  assert.equal(first.after.candidate_branch, recoveryBranch);

  const repeated = reconcileSentinelRecoveryRecord({
    record: first.after,
    remote: recoveryRemote(),
    now: "2026-08-28T18:03:00.000Z",
    expected_state_version: first.after.state_version,
    expected_lease_token: "lease-1",
  });
  assert.equal(repeated.action, "resume_validation");
  assert.equal(repeated.changed, false);
  assert.equal(repeated.after.state_version, first.after.state_version);
  assert.equal(repeated.after.candidate_branch, recoveryBranch);
});

Deno.test("ambiguous checkpoint publication retries the same branch idempotently", () => {
  const remote = recoveryRemote({ candidate_branch: null, candidate_sha: null });
  const first = reconcileSentinelRecoveryRecord({
    record: recoveryRecord(),
    remote,
    now: "2026-08-28T18:02:00.000Z",
  });
  assert.equal(first.action, "retry_checkpoint_push");
  assert.equal(first.changed, false);
  assert.equal(first.after.disposition, "active");
  assert.equal(first.after.candidate_branch, recoveryBranch);
  const repeated = reconcileSentinelRecoveryRecord({
    record: first.after,
    remote,
    now: "2026-08-28T18:03:00.000Z",
  });
  assert.equal(repeated.action, "retry_checkpoint_push");
  assert.equal(repeated.changed, false);
  assert.deepEqual(repeated.after, first.after);
});

Deno.test("recovery waits for remote evidence and rejects a non-deterministic recorded branch", () => {
  const absent = reconcileSentinelRecoveryRecord({
    record: recoveryRecord(),
    now: "2026-08-28T18:02:00.000Z",
  });
  assert.equal(absent.action, "await_evidence");
  assert.equal(absent.changed, false);

  const unsafe = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({ candidate_branch: "development" }),
    remote: recoveryRemote({ candidate_branch: null, candidate_sha: null }),
    now: "2026-08-28T18:02:00.000Z",
  });
  assert.equal(unsafe.action, "manual_required");
  assert.match(unsafe.after.reason ?? "", /not deterministic/u);
});

Deno.test("repeated delivery reconciliation records one terminal disposition and no duplicate delivery", () => {
  const record = recoveryRecord({ phase: "review_pending", state_version: 3 });
  const remote = recoveryRemote({
    deliveries: [{
      pull_request_number: 201,
      head_branch: recoveryBranch,
      head_sha: recoveryCandidateSha,
      base_branch: "development",
      head_contained_in_development: true,
      state: "closed",
      merged_at: "2026-08-28T18:04:00.000Z",
    }],
  });
  const first = reconcileSentinelRecoveryRecord({ record, remote, now: "2026-08-28T18:05:00.000Z" });
  assert.equal(first.action, "delivery_confirmed");
  assert.equal(first.after.phase, "delivered");
  assert.equal(first.after.disposition, "delivered");
  assert.equal(first.after.candidate_branch, recoveryBranch);
  const repeated = reconcileSentinelRecoveryRecord({
    record: first.after,
    remote,
    now: "2026-08-28T18:06:00.000Z",
  });
  assert.equal(repeated.action, "terminal_noop");
  assert.equal(repeated.changed, false);
  assert.equal(repeated.after.disposition, "delivered");
  assert.equal(repeated.after.state_version, first.after.state_version);
});

Deno.test("merged recovery delivery requires development ancestry proof", () => {
  const result = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({ phase: "review_pending", state_version: 3 }),
    remote: recoveryRemote({
      deliveries: [{
        pull_request_number: 201,
        head_branch: recoveryBranch,
        head_sha: recoveryCandidateSha,
        base_branch: "development",
        head_contained_in_development: false,
        state: "closed",
        merged_at: "2026-08-28T18:04:00.000Z",
      }],
    }),
    now: "2026-08-28T18:05:00.000Z",
  });
  assert.equal(result.action, "manual_required");
  assert.match(result.after.reason ?? "", /not proven in development ancestry/u);
});

Deno.test("linked canonical parents reconcile through the exact child checkpoint branch", () => {
  const childIdentity = { ...recoveryIdentity, source_revision: "b".repeat(64) };
  const childKey = sentinelRecoveryIdentityKey(childIdentity);
  const childBranch = sentinelRecoveryCandidateBranch(childIdentity);
  const parent = recoveryRecord({
    phase: "checkpoint_durable" as const,
    candidate_branch: childBranch,
    candidate_sha: recoveryCandidateSha,
    predecessor: childKey,
  });
  const remote = recoveryRemote({ candidate_branch: childBranch });
  const result = reconcileSentinelRecoveryRecord({
    record: parent,
    remote,
    now: "2026-08-28T18:05:00.000Z",
  });
  assert.equal(result.action, "resume_validation");
  assert.equal(result.changed, false);
  assert.equal(result.after.candidate_branch, childBranch);
  assert.equal(result.after.phase, "checkpoint_durable");
  // A recorded branch is deterministic only when it belongs to the linked
  // child identity; anything else stays a manual disposition.
  const unsafe = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({
      phase: "checkpoint_durable" as const,
      candidate_branch: "development",
      predecessor: childKey,
    }),
    remote: recoveryRemote({ candidate_branch: null, candidate_sha: null }),
    now: "2026-08-28T18:05:00.000Z",
  });
  assert.equal(unsafe.action, "manual_required");
  assert.match(unsafe.after.reason ?? "", /not deterministic/u);
});

Deno.test("a candidate already at development without a delivery record is manual", () => {
  const result = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({ phase: "recovery_pending" }),
    remote: recoveryRemote({ development_sha: recoveryCandidateSha, deliveries: [] }),
    now: "2026-08-28T18:05:00.000Z",
  });
  assert.equal(result.action, "manual_required");
  assert.equal(result.after.disposition, "manual_required");
  assert.match(result.after.reason ?? "", /already development/u);
});

Deno.test("duplicate delivery observations become one manual disposition and stay terminal", () => {
  const delivery = {
    pull_request_number: 201,
    head_branch: recoveryBranch,
    head_sha: recoveryCandidateSha,
    base_branch: "development" as const,
    head_contained_in_development: false,
    state: "open" as const,
    merged_at: null,
  };
  const remote = recoveryRemote({ deliveries: [delivery, { ...delivery, pull_request_number: 202 }] });
  const first = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({ phase: "review_pending", state_version: 3 }),
    remote,
    now: "2026-08-28T18:05:00.000Z",
  });
  assert.equal(first.action, "manual_required");
  assert.equal(first.after.disposition, "manual_required");
  const repeated = reconcileSentinelRecoveryRecord({
    record: first.after,
    remote,
    now: "2026-08-28T18:06:00.000Z",
  });
  assert.equal(repeated.action, "terminal_noop");
  assert.equal(repeated.changed, false);
  assert.equal(repeated.after.state_version, first.after.state_version);
});

Deno.test("recovery compare-and-swap rejects a stale lease or state version", () => {
  const current = recoveryRecord();
  const next = reconcileSentinelRecoveryRecord({
    record: current,
    remote: recoveryRemote({ candidate_branch: "sentinel/candidate-999999999", candidate_sha: recoveryCandidateSha }),
    now: "2026-08-28T18:02:00.000Z",
  }).after;
  assert.equal(next.disposition, "manual_required");
  assert.deepEqual(
    compareAndSwapSentinelRecoveryRecord({
      current,
      next,
      expected_state_version: 1,
      expected_lease_token: "lease-1",
    }),
    next,
  );
  assert.throws(
    () =>
      compareAndSwapSentinelRecoveryRecord({
        current,
        next,
        expected_state_version: 2,
        expected_lease_token: "lease-1",
      }),
    /lost its lease or observed a newer state/u,
  );
});

Deno.test("an empty candidate diff reaches an exact rejected disposition", () => {
  const result = reconcileSentinelRecoveryRecord({
    record: recoveryRecord({
      candidate_branch: null,
      candidate_sha: null,
      changed_files: [],
    }),
    remote: recoveryRemote({ candidate_branch: null, candidate_sha: null }),
    now: "2026-08-28T18:02:00.000Z",
  });
  assert.equal(result.action, "reject_no_candidate_diff");
  assert.equal(result.after.phase, "rejected");
  assert.equal(result.after.disposition, "rejected");
  assert.equal(result.after.reason, "rejected/no_candidate_diff");
});
