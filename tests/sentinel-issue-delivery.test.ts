import assert from "node:assert/strict";
import {
  createGitHubIssueJob,
  type GitHubIssueJobSource,
  parseGitHubIssueTimeLabel,
  renderGitHubIssueJobLedger,
  selectNextGitHubIssueJob,
} from "../scripts/sentinel/issues.ts";
import {
  evaluateIssueCompletionAction,
  isContainedDevelopmentComparison,
  isIssueDeliveryFailSafeRevert,
  isPullRequestMergeRefusalStatus,
  ISSUE_COMPLETION_EVIDENCE_TEXT,
  issuePullRequestMarker,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueSelectionReport,
  parseGitPushUpdates,
  parseSentinelCycleReport,
  renderIssueDeliveryEvidence,
  renderIssuePullRequestBody,
  selectDevelopmentPush,
} from "../scripts/sentinel/issue-delivery.ts";
import {
  closeIssueAfterCompletionEvidenceRevalidation,
  completionEvidenceSnapshotMatches,
  mergeDeliveryPullRequest,
} from "../scripts/sentinel/issue-delivery-reconcile.ts";
import type { GitHubIssueComment } from "../scripts/sentinel/github.ts";

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
  );
  assert.equal(selected?.timeLabel, "Time: <1 Day");
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
