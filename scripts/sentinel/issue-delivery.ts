const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const CHECKPOINT_BRANCH = /^sentinel\/candidate-[1-9][0-9]*(?:-[1-9][0-9]*)?$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const SAFE_URL = /^https:\/\/github\.com\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\/(?:pull|actions\/runs)\/[1-9][0-9]*$/u;

export const ISSUE_COMPLETION_EVIDENCE_TEXT =
  "Delivered, merged, and verified in production; issue closed as completed.";

export type GitHubIssueSelectionReport = Readonly<{
  schema_version: 1;
  issue_id: number;
  issue_number: number;
  fingerprint: string;
  body_sha256: string;
  comments: number;
  priority: "P2" | "P3";
  time_label: string;
  files: readonly string[];
  updated_at: string;
}>;

export type SentinelCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  candidate_sha: string | null;
  temporary_branch: string | null;
  status: string;
  stage: string;
  evidence_artifact_name: string | null;
}>;

export type GitHubIssuePullRequestRecord = Readonly<{
  schema_version: 1;
  issue_number: number;
  fingerprint: string;
  pull_request_number: number;
  pull_request_url: string;
  head_branch: string;
  head_sha: string;
  base_branch: "development";
  marker: string;
  reused: boolean;
}>;

export type GitHubIssueRetryPendingReport = Readonly<{
  schema_version: 1;
  issue_id: number;
  issue_number: number;
  fingerprint: string;
  phase: "failed_implementation" | "retry_checkpoint_resume_transient";
  implementation_status: "blocked";
  disposition: "retry_pending";
  retry_checkpoint: GitHubIssueRetryCheckpointReport | null;
}>;

export type GitHubIssueRetryCheckpointReport = Readonly<{
  branch: string;
  sha: string;
  base_sha: string;
}>;

export type SentinelRetryPendingCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  started_at: string;
  base_development_sha: string;
  candidate_sha: string;
  temporary_branch: string;
  status: "running" | "no_change";
  stage: "pushing_retry_pending_github_issue" | "complete";
  branch_disposition:
    | "runner_local_pending_review"
    | "development_docs_only_issue_retry_pending"
    | "remote_retained_issue_retry_pending";
  retry_checkpoint: GitHubIssueRetryCheckpointReport | null;
}>;

export type GitPushUpdate = Readonly<{
  localRef: string;
  localSha: string;
  remoteRef: string;
  remoteSha: string;
}>;

export type IssueDeliveryDisposition = "resolved" | "manual_required";
export type IssueProductionOutcome = "kept" | "rolled_back";
export type IssueCompletionAction =
  | "close_completed"
  | "leave_open_manual_required"
  | "leave_open_rolled_back"
  | "leave_open_failed"
  | "no_issue_delivery";

const record = (value: unknown): Record<string, unknown> | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;

const positiveInteger = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) > 0;
const nonNegativeInteger = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;
const isoTimestamp = (value: unknown): value is string =>
  typeof value === "string" && Number.isFinite(Date.parse(value)) && /^\d{4}-\d{2}-\d{2}T/u.test(value);

const parseRetryCheckpoint = (value: unknown): GitHubIssueRetryCheckpointReport | null => {
  if (value === null) return null;
  const checkpoint = record(value);
  if (
    !checkpoint || Object.keys(checkpoint).length !== 3 ||
    typeof checkpoint.branch !== "string" || !CHECKPOINT_BRANCH.test(checkpoint.branch) ||
    typeof checkpoint.sha !== "string" || !FULL_SHA.test(checkpoint.sha) ||
    typeof checkpoint.base_sha !== "string" || !FULL_SHA.test(checkpoint.base_sha) ||
    checkpoint.sha === checkpoint.base_sha
  ) throw new Error("Sentinel retry checkpoint report is invalid");
  return { branch: checkpoint.branch, sha: checkpoint.sha, base_sha: checkpoint.base_sha };
};

export const parseGitHubIssueSelectionReport = (value: unknown): GitHubIssueSelectionReport => {
  const selection = record(value);
  if (
    !selection || selection.schema_version !== 1 || !positiveInteger(selection.issue_id) ||
    !positiveInteger(selection.issue_number) || typeof selection.fingerprint !== "string" ||
    !SHA256.test(selection.fingerprint) || typeof selection.body_sha256 !== "string" ||
    !SHA256.test(selection.body_sha256) || !nonNegativeInteger(selection.comments) ||
    (selection.priority !== "P2" && selection.priority !== "P3") ||
    typeof selection.time_label !== "string" || selection.time_label.length === 0 || selection.time_label.length > 80 ||
    !Array.isArray(selection.files) || selection.files.length === 0 || selection.files.length > 32 ||
    !selection.files.every((path) =>
      typeof path === "string" && path.length > 0 && path.length <= 512 && !path.startsWith("/") &&
      !path.includes("\\") && !path.split("/").some((part) => part === "" || part === "." || part === "..")
    ) || !isoTimestamp(selection.updated_at)
  ) throw new Error("Sentinel GitHub issue selection report is invalid");
  return {
    schema_version: 1,
    issue_id: selection.issue_id,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    body_sha256: selection.body_sha256,
    comments: selection.comments,
    priority: selection.priority,
    time_label: selection.time_label,
    files: [...selection.files] as string[],
    updated_at: selection.updated_at,
  };
};

export const parseGitHubIssueRetryPendingReport = (
  value: unknown,
  expected: Readonly<{ issueId: number; issueNumber: number; fingerprint: string }>,
): GitHubIssueRetryPendingReport => {
  const report = record(value);
  if (
    !positiveInteger(expected.issueId) || !positiveInteger(expected.issueNumber) ||
    !SHA256.test(expected.fingerprint) || report?.schema_version !== 1 ||
    report.issue_id !== expected.issueId || report.issue_number !== expected.issueNumber ||
    report.fingerprint !== expected.fingerprint ||
    (report.phase !== "failed_implementation" && report.phase !== "retry_checkpoint_resume_transient") ||
    report.implementation_status !== "blocked" || report.disposition !== "retry_pending"
  ) {
    throw new Error("Sentinel retry-pending disposition does not match the exact issue selection");
  }
  const retryCheckpoint = parseRetryCheckpoint(report.retry_checkpoint);
  return {
    schema_version: 1,
    issue_id: expected.issueId,
    issue_number: expected.issueNumber,
    fingerprint: expected.fingerprint,
    phase: report.phase,
    implementation_status: "blocked",
    disposition: "retry_pending",
    retry_checkpoint: retryCheckpoint,
  };
};

export const parseSentinelRetryPendingCycleReport = (
  value: unknown,
  expected: Readonly<{
    runId: string;
    stage: SentinelRetryPendingCycleReport["stage"];
    status: SentinelRetryPendingCycleReport["status"];
    branchDispositions: readonly SentinelRetryPendingCycleReport["branch_disposition"][];
  }>,
): SentinelRetryPendingCycleReport => {
  const cycle = record(value);
  if (
    expected.runId.length === 0 || expected.runId.length > 200 ||
    !cycle || cycle.schema_version !== 1 || cycle.run_id !== expected.runId ||
    typeof cycle.started_at !== "string" || !isoTimestamp(cycle.started_at) ||
    typeof cycle.base_development_sha !== "string" || !FULL_SHA.test(cycle.base_development_sha) ||
    typeof cycle.candidate_sha !== "string" || !FULL_SHA.test(cycle.candidate_sha) ||
    typeof cycle.temporary_branch !== "string" || !SAFE_BRANCH.test(cycle.temporary_branch) ||
    cycle.status !== expected.status || cycle.stage !== expected.stage ||
    expected.branchDispositions.length === 0 ||
    !expected.branchDispositions.includes(
      cycle.branch_disposition as SentinelRetryPendingCycleReport["branch_disposition"],
    )
  ) {
    throw new Error("Sentinel retry-pending cycle report is invalid");
  }
  const retryCheckpoint = parseRetryCheckpoint(cycle.retry_checkpoint);
  if (
    (cycle.branch_disposition === "remote_retained_issue_retry_pending") !== (retryCheckpoint !== null)
  ) throw new Error("Sentinel retry-pending cycle checkpoint does not match its branch disposition");
  return {
    schema_version: 1,
    run_id: expected.runId,
    started_at: cycle.started_at,
    base_development_sha: cycle.base_development_sha,
    candidate_sha: cycle.candidate_sha,
    temporary_branch: cycle.temporary_branch,
    status: expected.status,
    stage: expected.stage,
    branch_disposition: cycle.branch_disposition as SentinelRetryPendingCycleReport["branch_disposition"],
    retry_checkpoint: retryCheckpoint,
  };
};

export const validateRetryPendingCheckpointPhaseBinding = (
  disposition: GitHubIssueRetryPendingReport,
  cycle: SentinelRetryPendingCycleReport,
): void => {
  const checkpoint = disposition.retry_checkpoint;
  if (disposition.phase === "failed_implementation") {
    if (
      checkpoint !== null &&
      (checkpoint.branch !== cycle.temporary_branch || checkpoint.base_sha !== cycle.base_development_sha)
    ) {
      throw new Error("Sentinel failed implementation checkpoint is not bound to the current attempt");
    }
    return;
  }
  if (
    checkpoint === null || cycle.branch_disposition !== "remote_retained_issue_retry_pending" ||
    checkpoint.branch === cycle.temporary_branch
  ) {
    throw new Error("Sentinel transient retry checkpoint is not bound to a prior attempt");
  }
};

export const parseSentinelCycleReport = (value: unknown): SentinelCycleReport => {
  const cycle = record(value);
  if (
    !cycle || cycle.schema_version !== 1 || typeof cycle.run_id !== "string" || cycle.run_id.length === 0 ||
    cycle.run_id.length > 200 ||
    (cycle.candidate_sha !== null &&
      (typeof cycle.candidate_sha !== "string" || !FULL_SHA.test(cycle.candidate_sha))) ||
    (cycle.temporary_branch !== null &&
      (typeof cycle.temporary_branch !== "string" || !SAFE_BRANCH.test(cycle.temporary_branch))) ||
    typeof cycle.status !== "string" || cycle.status.length === 0 || cycle.status.length > 80 ||
    typeof cycle.stage !== "string" || cycle.stage.length === 0 || cycle.stage.length > 120 ||
    (cycle.evidence_artifact_name !== null &&
      (typeof cycle.evidence_artifact_name !== "string" || cycle.evidence_artifact_name.length > 240))
  ) throw new Error("Sentinel cycle report is invalid");
  return {
    schema_version: 1,
    run_id: cycle.run_id,
    candidate_sha: cycle.candidate_sha,
    temporary_branch: cycle.temporary_branch,
    status: cycle.status,
    stage: cycle.stage,
    evidence_artifact_name: cycle.evidence_artifact_name,
  };
};

export const parseGitHubIssuePullRequestRecord = (value: unknown): GitHubIssuePullRequestRecord => {
  const pull = record(value);
  if (
    !pull || pull.schema_version !== 1 || !positiveInteger(pull.issue_number) ||
    typeof pull.fingerprint !== "string" || !SHA256.test(pull.fingerprint) ||
    !positiveInteger(pull.pull_request_number) || typeof pull.pull_request_url !== "string" ||
    !/^https:\/\/github\.com\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\/pull\/[1-9][0-9]*$/u.test(pull.pull_request_url) ||
    typeof pull.head_branch !== "string" || !SAFE_BRANCH.test(pull.head_branch) ||
    typeof pull.head_sha !== "string" || !FULL_SHA.test(pull.head_sha) || pull.base_branch !== "development" ||
    typeof pull.marker !== "string" || pull.marker.length < 40 || pull.marker.length > 240 ||
    typeof pull.reused !== "boolean"
  ) throw new Error("Sentinel GitHub issue pull-request record is invalid");
  return {
    schema_version: 1,
    issue_number: pull.issue_number,
    fingerprint: pull.fingerprint,
    pull_request_number: pull.pull_request_number,
    pull_request_url: pull.pull_request_url,
    head_branch: pull.head_branch,
    head_sha: pull.head_sha,
    base_branch: "development",
    marker: pull.marker,
    reused: pull.reused,
  };
};

export const parseGitPushUpdates = (value: string): GitPushUpdate[] => {
  const updates: GitPushUpdate[] = [];
  for (const line of value.replaceAll("\r\n", "\n").split("\n")) {
    if (line.length === 0) continue;
    const parts = line.split(" ");
    if (parts.length !== 4) throw new Error("Git pre-push input has an invalid shape");
    const [localRef, localSha, remoteRef, remoteSha] = parts as [string, string, string, string];
    if (
      (localRef !== "HEAD" && !localRef.startsWith("refs/")) || !remoteRef.startsWith("refs/") ||
      !FULL_SHA.test(localSha) || !FULL_SHA.test(remoteSha)
    ) throw new Error("Git pre-push input has invalid refs or SHAs");
    updates.push({ localRef, localSha, remoteRef, remoteSha });
  }
  return updates;
};

export const selectDevelopmentPush = (updates: readonly GitPushUpdate[]): GitPushUpdate | null => {
  const matches = updates.filter((update) => update.remoteRef === "refs/heads/development");
  if (matches.length > 1) throw new Error("Sentinel attempted multiple development updates in one push");
  return matches[0] ?? null;
};

export const isIssueDeliveryFailSafeRevert = (
  input: Readonly<{
    selection: GitHubIssueSelectionReport;
    cycle: SentinelCycleReport;
    pullRequest: GitHubIssuePullRequestRecord;
    pushedSha: string;
    parentSha: string;
  }>,
): boolean =>
  input.cycle.candidate_sha !== null && input.cycle.temporary_branch !== null &&
  input.pushedSha !== input.cycle.candidate_sha && input.parentSha === input.cycle.candidate_sha &&
  input.pullRequest.issue_number === input.selection.issue_number &&
  input.pullRequest.fingerprint === input.selection.fingerprint &&
  input.pullRequest.head_branch === input.cycle.temporary_branch &&
  input.pullRequest.head_sha === input.cycle.candidate_sha &&
  input.pullRequest.base_branch === "development";

export const issuePullRequestMarker = (selection: GitHubIssueSelectionReport): string =>
  `<!-- provider-sentinel:issue-pr:v1 issue=${selection.issue_number} fingerprint=${selection.fingerprint} -->`;

export const issueEvidenceMarker = (selection: GitHubIssueSelectionReport): string =>
  `<!-- provider-sentinel:issue-evidence:v1 issue=${selection.issue_number} fingerprint=${selection.fingerprint} -->`;

export const renderIssuePullRequestBody = (
  input: Readonly<{
    repository: string;
    selection: GitHubIssueSelectionReport;
    cycle: SentinelCycleReport;
    candidateSha: string;
    workflowRunUrl: string;
    validationReports: readonly string[];
    nativeReviewReports: readonly string[];
    replayReports: readonly string[];
    previewRevision: string | null;
    previewWorkflowRunId: number | null;
  }>,
): string => {
  if (!SAFE_REPOSITORY.test(input.repository) || !FULL_SHA.test(input.candidateSha)) {
    throw new Error("Sentinel pull-request evidence identity is invalid");
  }
  if (!SAFE_URL.test(input.workflowRunUrl)) throw new Error("Sentinel workflow URL is invalid");
  if (input.cycle.candidate_sha !== input.candidateSha || !input.cycle.temporary_branch) {
    throw new Error("Sentinel pull-request evidence does not match the cycle candidate");
  }
  const sorted = (values: readonly string[]): string[] =>
    [...new Set(values)].sort().filter((value) => /^[A-Za-z0-9._/-]{1,240}$/u.test(value));
  const validationReports = sorted(input.validationReports);
  const nativeReviewReports = sorted(input.nativeReviewReports);
  const replayReports = sorted(input.replayReports);
  return [
    issuePullRequestMarker(input.selection),
    "",
    `## Provider Sentinel deliverable for #${input.selection.issue_number}`,
    "",
    "This pull request is the single delivery record for the selected immutable GitHub issue snapshot. It intentionally does not use a closing keyword; Sentinel merges this pull request and closes the issue only after a verified production keep.",
    "",
    "### Linked issue evidence",
    "",
    `- Issue: [#${input.selection.issue_number}](https://github.com/${input.repository}/issues/${input.selection.issue_number})`,
    `- Issue fingerprint: \`${input.selection.fingerprint}\``,
    `- Issue body SHA-256: \`${input.selection.body_sha256}\``,
    `- Source updated: ${input.selection.updated_at}`,
    `- Priority: ${input.selection.priority}`,
    `- Estimate: ${input.selection.time_label}`,
    `- Candidate commit: \`${input.candidateSha}\``,
    `- Candidate branch: \`${input.cycle.temporary_branch}\``,
    "",
    "### Declared file scope",
    "",
    ...input.selection.files.map((path) => `- \`${path}\``),
    "",
    "### Supporting evidence",
    "",
    `- Provider Sentinel workflow: ${input.workflowRunUrl}`,
    `- Encrypted evidence artifact: \`${input.cycle.evidence_artifact_name ?? "assigned after run initialization"}\``,
    `- Preview revision: ${input.previewRevision ? `\`${input.previewRevision}\`` : "not applicable"}`,
    `- Preview deployment workflow run: ${input.previewWorkflowRunId ?? "not applicable"}`,
    `- Validation reports: ${
      validationReports.length ? validationReports.map((name) => `\`${name}\``).join(", ") : "none"
    }`,
    `- Native review reports: ${
      nativeReviewReports.length ? nativeReviewReports.map((name) => `\`${name}\``).join(", ") : "none"
    }`,
    `- Replay reports: ${replayReports.length ? replayReports.map((name) => `\`${name}\``).join(", ") : "none"}`,
    "",
    "Production deployment, monitoring, and final issue disposition are appended as a PR comment after the cycle settles.",
    "",
  ].join("\n");
};

export const evaluateIssueCompletionAction = (
  input: Readonly<{
    hasSelection: boolean;
    workflowFailed: boolean;
    disposition: IssueDeliveryDisposition | null;
    outcome: IssueProductionOutcome | null;
    pullRequestMerged: boolean;
    issueSnapshotMatches: boolean;
  }>,
): IssueCompletionAction => {
  if (!input.hasSelection) return "no_issue_delivery";
  if (input.workflowFailed) return "leave_open_failed";
  if (input.disposition === "manual_required") return "leave_open_manual_required";
  if (input.outcome === "rolled_back") return "leave_open_rolled_back";
  if (
    input.disposition === "resolved" && input.outcome === "kept" && input.pullRequestMerged &&
    input.issueSnapshotMatches
  ) return "close_completed";
  return "leave_open_failed";
};

/**
 * Pull-request merge responses that may indicate the head is already included
 * in the base branch, a merge already in progress, or a branch-protection
 * denial. Sentinel verifies the comparison before treating any of these as an
 * already-integrated delivery; every other status is a hard failure.
 */
export const isPullRequestMergeRefusalStatus = (status: number): boolean =>
  status === 403 || status === 405 || status === 409 || status === 422;

/**
 * The compare endpoint reports the head's state relative to the base. For a
 * `development...head` comparison, `behind` and `identical` both mean the head
 * commit is already contained in `development` (the state Sentinel pushes and
 * deploys), so an API merge refusal is only a formality.
 */
export const isContainedDevelopmentComparison = (status: string): boolean =>
  status === "behind" || status === "identical";

export const renderIssueDeliveryEvidence = (
  input: Readonly<{
    repository: string;
    selection: GitHubIssueSelectionReport;
    pullRequest: GitHubIssuePullRequestRecord;
    workflowRunUrl: string;
    action: IssueCompletionAction;
    cycleStatus: string;
    candidateSha: string;
    productionRevision: string | null;
    deploymentWorkflowRunId: number | null;
    promotionWorkflowRunId: number | null;
    monitoringDecision: "keep" | "rollback" | null;
  }>,
): string => {
  if (
    !SAFE_REPOSITORY.test(input.repository) || !SAFE_URL.test(input.workflowRunUrl) ||
    !FULL_SHA.test(input.candidateSha)
  ) {
    throw new Error("Sentinel final evidence identity is invalid");
  }
  if (
    input.pullRequest.issue_number !== input.selection.issue_number ||
    input.pullRequest.fingerprint !== input.selection.fingerprint ||
    input.pullRequest.head_sha !== input.candidateSha
  ) throw new Error("Sentinel final evidence does not match the issue pull request");
  const result = input.action === "close_completed"
    ? ISSUE_COMPLETION_EVIDENCE_TEXT
    : input.action === "leave_open_manual_required"
    ? "No autonomous deliverable was accepted; issue remains open for manual work."
    : input.action === "leave_open_rolled_back"
    ? "The candidate was rolled back; issue remains open."
    : "The cycle did not produce a verified deliverable; issue remains open.";
  return [
    issueEvidenceMarker(input.selection),
    "",
    "## Provider Sentinel delivery evidence",
    "",
    `- Result: ${result}`,
    `- Pull request: ${input.pullRequest.pull_request_url}`,
    `- Candidate commit: \`${input.candidateSha}\``,
    `- Workflow: ${input.workflowRunUrl}`,
    `- Cycle status: \`${input.cycleStatus}\``,
    `- Production revision: ${input.productionRevision ? `\`${input.productionRevision}\`` : "not retained"}`,
    `- Deployment workflow run: ${input.deploymentWorkflowRunId ?? "not available"}`,
    `- Promotion workflow run: ${input.promotionWorkflowRunId ?? "not available"}`,
    `- Monitoring decision: ${input.monitoringDecision ?? "not available"}`,
    "",
  ].join("\n");
};
