import assert from "node:assert/strict";
import {
  parseGitHubIssueTimeLabel,
  renderGitHubIssueJobLedger,
  selectNextGitHubIssueJob,
} from "../scripts/sentinel/issues.ts";
import {
  evaluateIssueCompletionAction,
  isIssueDeliveryFailSafeRevert,
  issuePullRequestMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseGitPushUpdates,
  parseSentinelCycleReport,
  renderIssueDeliveryEvidence,
  renderIssuePullRequestBody,
  selectDevelopmentPush,
} from "../scripts/sentinel/issue-delivery.ts";

const selection = parseGitHubIssueSelectionReport({
  schema_version: 1,
  issue_id: 5228586364,
  issue_number: 112,
  fingerprint: "a".repeat(64),
  body_sha256: "b".repeat(64),
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
    listOpenIssues: async () => [issue],
    getIssue: async () => issue,
    getIssueRelations: async () => relations,
    getRepositoryPermission: async () => "write" as const,
  };
  const selected = await selectNextGitHubIssueJob(
    source,
    "ubiquity/ai.ubq.fi",
    renderGitHubIssueJobLedger([]),
  );
  assert.equal(selected?.timeLabel, "Time: <1 Day");
});

Deno.test("pre-push parsing isolates exactly one development update", () => {
  const zero = "0".repeat(40);
  const updates = parseGitPushUpdates([
    `refs/heads/sentinel/candidate-123 ${"c".repeat(40)} refs/heads/sentinel/candidate-123 ${zero}`,
    `HEAD ${"c".repeat(40)} refs/heads/development ${"d".repeat(40)}`,
    "",
  ].join("\n"));
  assert.equal(selectDevelopmentPush(updates)?.localSha, "c".repeat(40));
  assert.throws(
    () => selectDevelopmentPush([...updates, updates[1]!]),
    /multiple development updates/,
  );
});

Deno.test("the issue PR gate permits only the exact one-parent fail-safe revert", () => {
  assert.equal(isIssueDeliveryFailSafeRevert({
    selection,
    cycle,
    pullRequest,
    pushedSha: "d".repeat(40),
    parentSha: "c".repeat(40),
  }), true);
  assert.equal(isIssueDeliveryFailSafeRevert({
    selection,
    cycle,
    pullRequest,
    pushedSha: "d".repeat(40),
    parentSha: "e".repeat(40),
  }), false);
});

Deno.test("one immutable issue snapshot renders one non-closing PR with bounded evidence", () => {
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
  assert.match(body, /validation-round-1\.json/);
  assert.doesNotMatch(body, /(?:close[sd]?|fixe[sd]?)\s+#112/iu);
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
  assert.match(evidence, /closed as completed/);
  assert.match(evidence, /Monitoring decision: keep/);
});
