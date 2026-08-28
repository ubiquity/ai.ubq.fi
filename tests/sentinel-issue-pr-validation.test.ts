import assert from "node:assert/strict";

import {
  ensureCandidateWorkflowValidation,
  parseCandidatePreviewEvidence,
  parseCandidateWorkflowValidationRecord,
  parseIssueCandidateDisposition,
  validateRetryPendingDevelopmentPush,
} from "../scripts/sentinel/issue-pr-pre-push.ts";
import { parseGitHubIssueSelectionReport } from "../scripts/sentinel/issue-delivery.ts";
import { type GitHubIssueJobLedgerEntry, renderGitHubIssueJobLedger } from "../scripts/sentinel/issues.ts";
import type { GitHubWorkflowDispatch, GitHubWorkflowRun, WaitForWorkflowOptions } from "../scripts/sentinel/github.ts";

const CANDIDATE_SHA = "b".repeat(40);
const CANDIDATE_BRANCH = "sentinel/candidate-issue-112";
const FINGERPRINT = "a".repeat(64);
const CORRELATION_ID = "sentinel-123e4567-e89b-42d3-a456-426614174000";
const DISPLAY_TITLE = `Deno Deploy ${CORRELATION_ID}`;
const BASE_SHA = "d".repeat(40);
const RETRY_SHA = "e".repeat(40);

const retrySelection = parseGitHubIssueSelectionReport({
  schema_version: 1,
  issue_id: 5228586364,
  issue_number: 112,
  fingerprint: FINGERPRINT,
  body_sha256: "f".repeat(64),
  comments: 2,
  priority: "P3",
  time_label: "Time: <1 Day",
  files: ["src/admin.ts"],
  updated_at: "2026-08-27T22:48:01Z",
});

const retryDisposition = {
  schema_version: 1,
  issue_id: retrySelection.issue_id,
  issue_number: retrySelection.issue_number,
  fingerprint: retrySelection.fingerprint,
  phase: "failed_implementation",
  implementation_status: "blocked",
  disposition: "retry_pending",
  retry_checkpoint: null,
};

const retryCycle = {
  schema_version: 1,
  run_id: "123456789",
  started_at: "2026-08-27T23:00:00Z",
  base_development_sha: BASE_SHA,
  candidate_sha: RETRY_SHA,
  temporary_branch: "sentinel/candidate-123456789-2",
  status: "running",
  stage: "pushing_retry_pending_github_issue",
  branch_disposition: "runner_local_pending_review",
  retry_checkpoint: null,
};

const retryRow = (overrides: Partial<GitHubIssueJobLedgerEntry> = {}): GitHubIssueJobLedgerEntry => ({
  issueId: retrySelection.issue_id,
  nodeId: "I_kwDOQoe6nc8AAAABN6XlfA",
  number: retrySelection.issue_number,
  fingerprint: retrySelection.fingerprint,
  bodySha256: retrySelection.body_sha256,
  comments: retrySelection.comments,
  sourceUpdatedAt: retrySelection.updated_at,
  recordedAt: "2026-08-27T23:20:00Z",
  baseSha: BASE_SHA,
  checkpoint: null,
  title: "Retry the bounded issue",
  disposition: "retry_pending" as const,
  ...overrides,
});

const successfulRun = (id: number, overrides: Partial<GitHubWorkflowRun> = {}): GitHubWorkflowRun => ({
  id,
  headSha: CANDIDATE_SHA,
  status: "completed",
  conclusion: "success",
  htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${id}`,
  createdAt: "2026-08-27T12:00:00Z",
  displayTitle: DISPLAY_TITLE,
  ...overrides,
});

const dispatch = (runId: number): GitHubWorkflowDispatch => ({
  runId,
  runUrl: `https://api.github.com/repos/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
  htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/actions/runs/${runId}`,
});

Deno.test("issue candidate evidence parsers bind disposition, preview, and retry evidence to exact identities", () => {
  assert.deepEqual(
    parseCandidatePreviewEvidence({
      git_sha: CANDIDATE_SHA,
      revision: "preview-revision-112",
      workflow_run_id: 71,
      replay_base_url: "https://preview.example.test",
    }, CANDIDATE_SHA),
    {
      gitSha: CANDIDATE_SHA,
      revision: "preview-revision-112",
      workflowRunId: 71,
    },
  );
  assert.throws(
    () =>
      parseCandidatePreviewEvidence({
        git_sha: "c".repeat(40),
        revision: "preview-revision-112",
        workflow_run_id: 71,
      }, CANDIDATE_SHA),
    /does not match the exact issue candidate/,
  );

  assert.equal(
    parseIssueCandidateDisposition({
      schema_version: 1,
      issue_number: 112,
      fingerprint: FINGERPRINT,
      disposition: "resolved",
    }, { issueNumber: 112, fingerprint: FINGERPRINT }),
    "resolved",
  );
  assert.throws(
    () =>
      parseIssueCandidateDisposition({
        schema_version: 1,
        issue_number: 113,
        fingerprint: FINGERPRINT,
        disposition: "resolved",
      }, { issueNumber: 112, fingerprint: FINGERPRINT }),
    /does not match the exact selection/,
  );

  const persisted = {
    schema_version: 1 as const,
    source: "build_only" as const,
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 72,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  };
  assert.deepEqual(
    parseCandidateWorkflowValidationRecord(persisted, {
      candidateSha: CANDIDATE_SHA,
      candidateBranch: CANDIDATE_BRANCH,
    }),
    persisted,
  );
  assert.throws(
    () =>
      parseCandidateWorkflowValidationRecord({ ...persisted, git_sha: "c".repeat(40) }, {
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
      }),
    /does not match the exact candidate/,
  );
});

Deno.test("retry-pending development pushes are exact docs-only commits with no delivery candidate", () => {
  const baseInput: Parameters<typeof validateRetryPendingDevelopmentPush>[0] = {
    workflowRunId: "123456789",
    update: {
      localRef: "HEAD",
      localSha: RETRY_SHA,
      remoteRef: "refs/heads/development",
      remoteSha: BASE_SHA,
    },
    selection: retrySelection,
    cycleValue: retryCycle,
    dispositionValue: retryDisposition,
    commitParents: [BASE_SHA],
    changedPaths: ["docs/sentinel-issue-jobs.md"],
    parentLedgerMarkdown: renderGitHubIssueJobLedger([]),
    pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow()]),
  };
  assert.doesNotThrow(() => validateRetryPendingDevelopmentPush(baseInput));

  const checkpoint = {
    branch: retryCycle.temporary_branch,
    sha: "1".repeat(40),
    base_sha: BASE_SHA,
  };
  const checkpointInput: Parameters<typeof validateRetryPendingDevelopmentPush>[0] = {
    ...baseInput,
    cycleValue: {
      ...retryCycle,
      branch_disposition: "remote_retained_issue_retry_pending",
      retry_checkpoint: checkpoint,
    },
    dispositionValue: { ...retryDisposition, retry_checkpoint: checkpoint },
    pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow({
      checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: checkpoint.base_sha },
    })]),
  };
  assert.doesNotThrow(() => validateRetryPendingDevelopmentPush(checkpointInput));

  const checkpointMismatchInputs: Array<
    readonly [Partial<Parameters<typeof validateRetryPendingDevelopmentPush>[0]>, RegExp]
  > = [
    [{
      pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow({
        checkpoint: { branch: "sentinel/candidate-123456780", sha: checkpoint.sha, baseSha: checkpoint.base_sha },
      })]),
    }, /exact pending ledger row/],
    [{
      pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow({
        checkpoint: { branch: checkpoint.branch, sha: "2".repeat(40), baseSha: checkpoint.base_sha },
      })]),
    }, /exact pending ledger row/],
    [{
      pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow({
        baseSha: "3".repeat(40),
        checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, baseSha: "3".repeat(40) },
      })]),
    }, /exact pending ledger row/],
    [{ dispositionValue: retryDisposition }, /exact pending ledger row/],
    [{ cycleValue: retryCycle }, /exact pending ledger row/],
    [{
      cycleValue: {
        ...retryCycle,
        temporary_branch: "sentinel/candidate-123456780",
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, branch: "sentinel/candidate-123456780" },
      },
    }, /exact pending ledger row/],
    [{
      cycleValue: {
        ...retryCycle,
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, sha: "2".repeat(40) },
      },
    }, /exact pending ledger row/],
    [{
      cycleValue: {
        ...retryCycle,
        branch_disposition: "remote_retained_issue_retry_pending",
        retry_checkpoint: { ...checkpoint, base_sha: "3".repeat(40) },
      },
    }, /checkpoint does not match its branch disposition/],
    [{
      cycleValue: {
        ...retryCycle,
        branch_disposition: "development_docs_only_issue_retry_pending",
        retry_checkpoint: checkpoint,
      },
    }, /cycle report is invalid/],
  ];
  for (const [overrides, pattern] of checkpointMismatchInputs) {
    assert.throws(
      () => validateRetryPendingDevelopmentPush({ ...checkpointInput, ...overrides }),
      pattern,
    );
  }

  const priorPending = retryRow({
    fingerprint: "9".repeat(64),
    bodySha256: "8".repeat(64),
    comments: 1,
    sourceUpdatedAt: "2026-08-27T22:00:00Z",
    recordedAt: "2026-08-27T23:05:00Z",
    baseSha: "c".repeat(40),
  });
  assert.doesNotThrow(() =>
    validateRetryPendingDevelopmentPush({
      ...baseInput,
      parentLedgerMarkdown: renderGitHubIssueJobLedger([priorPending]),
    })
  );
  const retainedCheckpointPending = {
    ...priorPending,
    checkpoint: {
      branch: "sentinel/candidate-123456700",
      sha: "5".repeat(40),
      baseSha: priorPending.baseSha,
    },
  };
  assert.doesNotThrow(() =>
    validateRetryPendingDevelopmentPush({
      ...baseInput,
      parentLedgerMarkdown: renderGitHubIssueJobLedger([retainedCheckpointPending]),
      pushedLedgerMarkdown: renderGitHubIssueJobLedger([
        { ...retainedCheckpointPending, disposition: "manual_required" },
        retryRow(),
      ]),
    })
  );

  const invalidInputs: Array<readonly [Partial<Parameters<typeof validateRetryPendingDevelopmentPush>[0]>, RegExp]> = [
    [{ dispositionValue: { ...retryDisposition, fingerprint: "7".repeat(64) } }, /exact issue selection/],
    [{ commitParents: [BASE_SHA, "c".repeat(40)] }, /one-parent commit/],
    [{ commitParents: ["c".repeat(40)] }, /selected development base/],
    [{ changedPaths: ["docs/sentinel-issue-jobs.md", "src/admin.ts"] }, /only the issue-job ledger/],
    [{ pushedLedgerMarkdown: renderGitHubIssueJobLedger([]) }, /exact pending ledger row/],
    [
      { pushedLedgerMarkdown: renderGitHubIssueJobLedger([retryRow({ disposition: "resolved" })]) },
      /pending ledger row/,
    ],
    [{ cycleValue: { ...retryCycle, candidate_sha: "c".repeat(40) } }, /exact cycle commit/],
  ];
  for (const [overrides, pattern] of invalidInputs) {
    assert.throws(() => validateRetryPendingDevelopmentPush({ ...baseInput, ...overrides }), pattern);
  }

  const unrelated = retryRow({
    issueId: 500,
    nodeId: "I_kwDOIssue500",
    number: 500,
    fingerprint: "6".repeat(64),
    bodySha256: "5".repeat(64),
    comments: 0,
    sourceUpdatedAt: "2026-08-20T00:00:00Z",
    recordedAt: "2026-08-21T00:00:00Z",
    baseSha: "4".repeat(40),
    title: "Unrelated terminal issue",
    disposition: "resolved",
  });
  assert.throws(
    () =>
      validateRetryPendingDevelopmentPush({
        ...baseInput,
        parentLedgerMarkdown: renderGitHubIssueJobLedger([unrelated]),
      }),
    /unrelated issue-job ledger rows/,
  );
  assert.throws(
    () =>
      validateRetryPendingDevelopmentPush({
        ...baseInput,
        parentLedgerMarkdown: renderGitHubIssueJobLedger([
          retryRow({ recordedAt: "2026-08-28T00:00:00Z" }),
        ]),
      }),
    /timestamp backwards/,
  );
});

Deno.test("resolved issue candidates reuse and verify the exact successful preview workflow", async () => {
  const waits: WaitForWorkflowOptions[] = [];
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: () => {
        throw new Error("resolved preview evidence must not dispatch another workflow");
      },
      waitForWorkflow: (options) => {
        waits.push(options);
        return Promise.resolve(successfulRun(81));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "resolved",
    preview: { gitSha: CANDIDATE_SHA, revision: "preview-revision-112", workflowRunId: 81 },
    existingBuildValidation: null,
  });

  assert.deepEqual(waits, [{ runId: 81, headSha: CANDIDATE_SHA }]);
  assert.deepEqual(result, {
    schema_version: 1,
    source: "preview",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 81,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  });
});

Deno.test("resolved issue candidates cannot reach development without preview workflow evidence", async () => {
  await assert.rejects(
    () =>
      ensureCandidateWorkflowValidation({
        client: {
          dispatchWorkflow: () => Promise.resolve(dispatch(82)),
          waitForWorkflow: () => Promise.resolve(successfulRun(82)),
        },
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
        disposition: "resolved",
        preview: null,
        existingBuildValidation: null,
      }),
    /require exact preview workflow evidence/,
  );
});

Deno.test("manual-required issue candidates dispatch an exact non-preview build and wait for its success", async () => {
  let dispatched = false;
  let waited = false;
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: (workflow, ref, inputs) => {
        dispatched = true;
        assert.equal(workflow, "deno-deploy.yml");
        assert.equal(ref, CANDIDATE_BRANCH);
        assert.deepEqual(inputs, {
          deploy_preview: false,
          sentinel_build_only: true,
          sentinel_correlation_id: CORRELATION_ID,
        });
        return Promise.resolve(dispatch(91));
      },
      waitForWorkflow: (options) => {
        waited = true;
        assert.deepEqual(options, {
          runId: 91,
          headSha: CANDIDATE_SHA,
          displayTitle: DISPLAY_TITLE,
        });
        return Promise.resolve(successfulRun(91));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "manual_required",
    preview: null,
    existingBuildValidation: null,
    createCorrelationId: () => CORRELATION_ID,
  });

  assert.equal(dispatched, true);
  assert.equal(waited, true);
  assert.deepEqual(result, {
    schema_version: 1,
    source: "build_only",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 91,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  });
});

Deno.test("manual candidate validation rejects a response for any workflow run other than the dispatched run", async () => {
  await assert.rejects(
    () =>
      ensureCandidateWorkflowValidation({
        client: {
          dispatchWorkflow: () => Promise.resolve(dispatch(101)),
          waitForWorkflow: () => Promise.resolve(successfulRun(102)),
        },
        candidateSha: CANDIDATE_SHA,
        candidateBranch: CANDIDATE_BRANCH,
        disposition: "manual_required",
        preview: null,
        existingBuildValidation: null,
        createCorrelationId: () => CORRELATION_ID,
      }),
    /wrong workflow run ID/,
  );
});

Deno.test("manual candidate validation reuses exact persisted build evidence after a hook retry", async () => {
  const evidence = parseCandidateWorkflowValidationRecord({
    schema_version: 1,
    source: "build_only",
    git_sha: CANDIDATE_SHA,
    head_branch: CANDIDATE_BRANCH,
    workflow_run_id: 111,
    correlation_id: CORRELATION_ID,
    display_title: DISPLAY_TITLE,
  }, { candidateSha: CANDIDATE_SHA, candidateBranch: CANDIDATE_BRANCH });
  let waited = false;
  const result = await ensureCandidateWorkflowValidation({
    client: {
      dispatchWorkflow: () => {
        throw new Error("an exact prior build validation must not be dispatched twice");
      },
      waitForWorkflow: (options) => {
        waited = true;
        assert.deepEqual(options, {
          runId: 111,
          headSha: CANDIDATE_SHA,
          displayTitle: DISPLAY_TITLE,
        });
        return Promise.resolve(successfulRun(111));
      },
    },
    candidateSha: CANDIDATE_SHA,
    candidateBranch: CANDIDATE_BRANCH,
    disposition: "manual_required",
    preview: null,
    existingBuildValidation: evidence,
  });

  assert.equal(waited, true);
  assert.deepEqual(result, evidence);
});
