import {
  type GitHubIssuePullRequestRecord,
  type GitHubIssueSelectionReport,
  type GitPushUpdate,
  isIssueDeliveryFailSafeRevert,
  issuePullRequestMarker,
  parseGitHubIssueManualRequiredReport,
  parseGitHubIssueManualRequiredRetainedCheckpointReport,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueRetryPendingReport,
  parseGitHubIssueSelectionReportAny,
  parseGitPushUpdates,
  parseSentinelCycleReport,
  parseSentinelManualRequiredCycleReport,
  parseSentinelManualRequiredRetainedCheckpointCycleReport,
  parseSentinelRetryPendingCycleReport,
  renderIssuePullRequestBody,
  selectDevelopmentPush,
  selectManualCheckpointPush,
  selectRetryCheckpointPush,
  validateRetryPendingCheckpointPhaseBinding,
  verifyFrozenIssuePlanDigest,
} from "./issue-delivery.ts";
import { parseGitHubIssueJobLedger, renderGitHubIssueJobLedger } from "./issues.ts";
import { parseSentinelRecoveryRecord } from "./recovery.ts";
import {
  GitHubActionsClient,
  type GitHubWorkflowDispatch,
  type GitHubWorkflowRun,
  type WaitForWorkflowOptions,
} from "./github.ts";
import { SENTINEL_POLICY } from "./policy.ts";

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const ZERO_SHA = "0".repeat(40);
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const SAFE_REVISION = /^[A-Za-z0-9_-]{1,200}$/u;
const SENTINEL_DEPLOYMENT_CORRELATION =
  /^sentinel-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u;

export type PullRequest = Readonly<{
  number: number;
  htmlUrl: string;
  state: "open" | "closed";
  mergedAt: string | null;
  body: string;
  headRef: string;
  headSha: string;
  baseRef: string;
}>;

export type CandidatePreviewEvidence = Readonly<{
  gitSha: string;
  revision: string;
  workflowRunId: number;
}>;

export type CandidateWorkflowValidationRecord = Readonly<{
  schema_version: 1;
  source: "preview" | "build_only";
  git_sha: string;
  head_branch: string;
  workflow_run_id: number;
  correlation_id: string;
  display_title: string;
}>;

type CandidateWorkflowValidationClient = Readonly<{
  dispatchWorkflow: (
    workflow: string,
    ref: string,
    inputs?: Readonly<Record<string, string | boolean>>,
  ) => Promise<GitHubWorkflowDispatch>;
  waitForWorkflow: (options: WaitForWorkflowOptions) => Promise<GitHubWorkflowRun>;
}>;

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required for Sentinel issue PR delivery`);
  return value;
};

const readJson = async (path: string): Promise<unknown> => JSON.parse(await Deno.readTextFile(path));

const optionalJson = async (path: string): Promise<unknown | null> => {
  try {
    return await readJson(path);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return null;
    throw error;
  }
};

const gitOutput = async (args: readonly string[], cwd?: string): Promise<string> => {
  const executable = Deno.env.get("SENTINEL_REAL_GIT")?.trim() || "git";
  const result = await new Deno.Command(executable, {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
  }).output();
  if (!result.success) {
    const stderr = new TextDecoder().decode(result.stderr).trim();
    throw new Error(
      `Git command failed: ${executable} ${args.join(" ")}${stderr ? `: ${stderr.slice(0, 500)}` : ""}`,
    );
  }
  return new TextDecoder().decode(result.stdout);
};

const git = async (args: readonly string[], cwd?: string): Promise<string> => (await gitOutput(args, cwd)).trim();

const githubRequest = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
): Promise<unknown> => {
  const response = await fetch(`https://api.github.com/repos/${repository}${path}`, {
    ...init,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": API_VERSION,
      ...(init.body === undefined ? {} : { "Content-Type": "application/json" }),
      ...init.headers,
    },
    signal: AbortSignal.timeout(30_000),
  });
  const text = await response.text();
  if (!response.ok) {
    throw new Error(
      `GitHub API ${init.method ?? "GET"} ${path} failed with HTTP ${response.status}: ${text.slice(0, 500)}`,
    );
  }
  return text.length === 0 ? null : JSON.parse(text);
};

const parsePullRequest = (value: unknown): PullRequest => {
  const pull = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  const head = pull?.head && typeof pull.head === "object" && !Array.isArray(pull.head)
    ? pull.head as Record<string, unknown>
    : null;
  const base = pull?.base && typeof pull.base === "object" && !Array.isArray(pull.base)
    ? pull.base as Record<string, unknown>
    : null;
  if (
    !pull || !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
    typeof pull.html_url !== "string" || !/^https:\/\/github\.com\//u.test(pull.html_url) ||
    (pull.state !== "open" && pull.state !== "closed") ||
    (pull.merged_at !== null && typeof pull.merged_at !== "string") ||
    (pull.body !== null && typeof pull.body !== "string") || !head || !base ||
    typeof head.ref !== "string" || !SAFE_BRANCH.test(head.ref) ||
    typeof head.sha !== "string" || !FULL_SHA.test(head.sha) ||
    typeof base.ref !== "string" || !SAFE_BRANCH.test(base.ref)
  ) {
    throw new Error("GitHub returned an invalid pull request");
  }
  return {
    number: pull.number as number,
    htmlUrl: pull.html_url,
    state: pull.state,
    mergedAt: pull.merged_at as string | null,
    body: (pull.body as string | null) ?? "",
    headRef: head.ref,
    headSha: head.sha,
    baseRef: base.ref,
  };
};

const listPullRequests = async (token: string, repository: string): Promise<PullRequest[]> => {
  const pulls: PullRequest[] = [];
  for (let page = 1; page <= 10; page++) {
    const value = await githubRequest(
      token,
      repository,
      `/pulls?state=all&per_page=100&page=${page}&sort=created&direction=desc`,
    );
    if (!Array.isArray(value)) throw new Error("GitHub pull-request listing is invalid");
    pulls.push(...value.map(parsePullRequest));
    if (value.length < 100) return pulls;
  }
  throw new Error("Sentinel pull-request lookup exceeded its pagination limit");
};

const ensureRemoteCandidateBranch = async (branch: string, candidateSha: string): Promise<void> => {
  const remote = await git(["ls-remote", "--heads", "origin", `refs/heads/${branch}`]);
  const remoteSha = remote.length === 0 ? null : remote.split(/\s+/u)[0] ?? null;
  if (remoteSha === candidateSha) return;
  if (remoteSha !== null && !FULL_SHA.test(remoteSha)) {
    throw new Error("Remote Sentinel candidate branch has an invalid SHA");
  }
  await git(["push", "--no-verify", "origin", `HEAD:refs/heads/${branch}`]);
  const confirmed = await git(["ls-remote", "--heads", "origin", `refs/heads/${branch}`]);
  if ((confirmed.split(/\s+/u)[0] ?? null) !== candidateSha) {
    throw new Error("Sentinel candidate branch did not reach the expected remote SHA");
  }
};

const reportNames = async (reportsDir: string, pattern: RegExp): Promise<string[]> => {
  const names: string[] = [];
  for await (const entry of Deno.readDir(reportsDir)) {
    if (entry.isFile && pattern.test(entry.name)) names.push(entry.name);
  }
  return names.sort();
};

export const parseCandidatePreviewEvidence = (
  value: unknown,
  candidateSha: string,
): CandidatePreviewEvidence => {
  const report = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  if (
    !FULL_SHA.test(candidateSha) || report?.git_sha !== candidateSha ||
    typeof report.revision !== "string" || !SAFE_REVISION.test(report.revision) ||
    !Number.isSafeInteger(report.workflow_run_id) || (report.workflow_run_id as number) <= 0
  ) {
    throw new Error("Sentinel preview evidence does not match the exact issue candidate");
  }
  return {
    gitSha: candidateSha,
    revision: report.revision,
    workflowRunId: report.workflow_run_id as number,
  };
};

const optionalPreviewEvidence = async (
  reportsDir: string,
  candidateSha: string,
): Promise<CandidatePreviewEvidence | null> => {
  const names = await reportNames(reportsDir, /^preview-deployment-round-[1-3]\.json$/u);
  const latest = names.at(-1);
  return latest ? parseCandidatePreviewEvidence(await readJson(`${reportsDir}/${latest}`), candidateSha) : null;
};

export const parseIssueCandidateDisposition = (
  value: unknown,
  expected: Readonly<{ issueNumber: number; fingerprint: string }>,
): "resolved" | "manual_required" => {
  const report = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  if (
    report?.schema_version !== 1 || report.issue_number !== expected.issueNumber ||
    report.fingerprint !== expected.fingerprint ||
    (report.disposition !== "resolved" && report.disposition !== "manual_required")
  ) {
    throw new Error("Sentinel GitHub issue disposition does not match the exact selection");
  }
  return report.disposition;
};

const ledgerEntryMatchesSelection = (
  entry: ReturnType<typeof parseGitHubIssueJobLedger>[number],
  selection: GitHubIssueSelectionReport,
): boolean =>
  entry.issueId === selection.issue_id && entry.number === selection.issue_number &&
  entry.fingerprint === selection.fingerprint && entry.bodySha256 === selection.body_sha256 &&
  entry.comments === selection.comments && entry.sourceUpdatedAt === selection.updated_at;

/**
 * All non-delivery issue outcomes can change only the selected ledger row.
 * Their report and cycle parsers remain phase-specific; this helper shares
 * only the deterministic ledger replacement rule.
 */
const validateLedgerOnlyIssueDisposition = (
  input: Readonly<{
    label: string;
    selection: GitHubIssueSelectionReport;
    disposition: "retry_pending" | "manual_required";
    checkpoint: Readonly<{ branch: string; sha: string; baseSha: string }> | null;
    expectedBaseSha: string;
    parentLedgerMarkdown: string;
    pushedLedgerMarkdown: string;
  }>,
): void => {
  const parentEntries = parseGitHubIssueJobLedger(input.parentLedgerMarkdown);
  const pushedEntries = parseGitHubIssueJobLedger(input.pushedLedgerMarkdown);
  const pushedMatches = pushedEntries.filter((entry) => ledgerEntryMatchesSelection(entry, input.selection));
  if (
    pushedMatches.length !== 1 || pushedMatches[0]!.disposition !== input.disposition ||
    pushedMatches[0]!.baseSha !== input.expectedBaseSha ||
    JSON.stringify(pushedMatches[0]!.checkpoint) !== JSON.stringify(input.checkpoint)
  ) {
    throw new Error(
      `Sentinel ${input.label} push has no exact ${
        input.disposition === "retry_pending" ? "pending" : "manual"
      } ledger row`,
    );
  }
  const priorPending = parentEntries.filter((entry) =>
    entry.issueId === input.selection.issue_id && entry.number === input.selection.issue_number &&
    entry.disposition === "retry_pending"
  );
  if (priorPending.length > 1) {
    throw new Error(`Sentinel ${input.label} push has multiple prior pending rows for the selected issue`);
  }
  if (
    priorPending.length === 1 &&
    Date.parse(priorPending[0]!.recordedAt) > Date.parse(pushedMatches[0]!.recordedAt)
  ) {
    throw new Error(`Sentinel ${input.label} push moved the retry timestamp backwards`);
  }
  const replacedSelectedRetained = parentEntries.filter((entry) =>
    entry.disposition === "checkpoint_retained" && ledgerEntryMatchesSelection(entry, input.selection)
  );
  const parentUnrelated = parentEntries.filter((entry) =>
    !priorPending.includes(entry) && !replacedSelectedRetained.includes(entry)
  );
  const retainedPriorCheckpoints = priorPending
    .filter((entry) => entry.fingerprint !== input.selection.fingerprint && entry.checkpoint !== null)
    .map((entry) => ({ ...entry, disposition: "checkpoint_retained" as const }));
  const pushedUnrelated = pushedEntries.filter((entry) => entry !== pushedMatches[0]);
  const expectedPushedUnrelated = parseGitHubIssueJobLedger(
    renderGitHubIssueJobLedger([...parentUnrelated, ...retainedPriorCheckpoints]),
  );
  if (JSON.stringify(expectedPushedUnrelated) !== JSON.stringify(pushedUnrelated)) {
    throw new Error(`Sentinel ${input.label} push changed unrelated issue-job ledger rows`);
  }
};

type IssueLedgerPushValidationInput = Readonly<{
  workflowRunId: string;
  updates: readonly GitPushUpdate[];
  atomicPush: boolean;
  checkpointLeaseSha: string | null;
  selection: GitHubIssueSelectionReport;
  cycleValue: unknown;
  dispositionValue: unknown;
  commitParents: readonly string[];
  changedPaths: readonly string[];
  parentLedgerMarkdown: string;
  pushedLedgerMarkdown: string;
}>;

type NativeManualLedgerPushValidationInput =
  & IssueLedgerPushValidationInput
  & Readonly<{
    workflowRunAttempt: number;
  }>;

export const validateRetryPendingDevelopmentPush = (
  input: IssueLedgerPushValidationInput,
): void => {
  const update = selectDevelopmentPush(input.updates);
  if (!update) throw new Error("Sentinel retry-pending push has no development update");
  const disposition = parseGitHubIssueRetryPendingReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = parseSentinelRetryPendingCycleReport(input.cycleValue, {
    runId: input.workflowRunId,
    status: "running",
    stage: "pushing_retry_pending_github_issue",
    branchDispositions: [
      "runner_local_pending_review",
      "runner_local_atomic_push_in_flight",
      "remote_retained_issue_retry_pending",
      "remote_retained_atomic_push_in_flight",
    ],
  });
  if (
    cycle.candidate_sha !== update.localSha || cycle.base_development_sha !== update.remoteSha ||
    !cycle.temporary_branch.startsWith("sentinel/candidate-")
  ) {
    throw new Error("Sentinel retry-pending push does not match the exact cycle commit");
  }
  if (input.commitParents.length !== 1 || input.commitParents[0] !== update.remoteSha) {
    throw new Error("Sentinel retry-pending push must be a one-parent commit on the selected development base");
  }
  if (
    input.changedPaths.length !== 1 || input.changedPaths[0] !== SENTINEL_POLICY.paths.issueJobLedger
  ) {
    throw new Error("Sentinel retry-pending push must change only the issue-job ledger");
  }

  const expectedCheckpoint = disposition.retry_checkpoint === null ? null : {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  if (JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint)) {
    throw new Error("Sentinel retry-pending push has no exact pending ledger row");
  }
  validateLedgerOnlyIssueDisposition({
    label: "retry-pending",
    selection: input.selection,
    disposition: "retry_pending",
    checkpoint: expectedCheckpoint,
    expectedBaseSha: expectedCheckpoint?.baseSha ?? update.remoteSha,
    parentLedgerMarkdown: input.parentLedgerMarkdown,
    pushedLedgerMarkdown: input.pushedLedgerMarkdown,
  });
  validateRetryPendingCheckpointPhaseBinding(disposition, cycle);
  if (disposition.retry_checkpoint === null) {
    if (input.updates.length !== 1) {
      throw new Error("Sentinel docs-only retry push has unexpected ref updates");
    }
  } else {
    if (!input.atomicPush || input.updates.length !== 2) {
      throw new Error("Sentinel checkpoint retry push must be one atomic two-ref push");
    }
    const checkpointUpdate = selectRetryCheckpointPush(input.updates, disposition.retry_checkpoint);
    if (input.checkpointLeaseSha !== checkpointUpdate.remoteSha) {
      throw new Error("Sentinel checkpoint retry push has no exact force-with-lease");
    }
    if (
      disposition.phase === "failed_implementation" &&
      cycle.branch_disposition !== "runner_local_pending_review" &&
      cycle.branch_disposition !== "runner_local_atomic_push_in_flight"
    ) {
      throw new Error("Sentinel new retry checkpoint must remain local until the atomic push");
    }
  }
};

/**
 * A failed resume keeps an already-published retry checkpoint. It can publish
 * only its docs-only manual disposition, never another candidate ref or PR.
 */
export const validateManualRequiredRetainedCheckpointDevelopmentPush = (
  input: IssueLedgerPushValidationInput,
): void => {
  const update = selectDevelopmentPush(input.updates);
  if (!update) throw new Error("Sentinel retained-checkpoint manual push has no development update");
  const disposition = parseGitHubIssueManualRequiredRetainedCheckpointReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = parseSentinelManualRequiredRetainedCheckpointCycleReport(input.cycleValue, {
    runId: input.workflowRunId,
    status: "running",
    stage: "pushing_manual_github_issue",
    branchDisposition: "runner_local_pending_review",
  });
  if (
    cycle.candidate_sha !== update.localSha || cycle.base_development_sha !== update.remoteSha ||
    JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint)
  ) {
    throw new Error("Sentinel retained-checkpoint manual push does not match the exact cycle commit");
  }
  if (input.commitParents.length !== 1 || input.commitParents[0] !== update.remoteSha) {
    throw new Error(
      "Sentinel retained-checkpoint manual push must be a one-parent commit on the selected development base",
    );
  }
  if (
    input.changedPaths.length !== 1 || input.changedPaths[0] !== SENTINEL_POLICY.paths.issueJobLedger
  ) {
    throw new Error("Sentinel retained-checkpoint manual push must change only the issue-job ledger");
  }
  if (input.atomicPush || input.updates.length !== 1 || input.checkpointLeaseSha !== null) {
    throw new Error("Sentinel retained-checkpoint manual push must be one docs-only development update");
  }
  const checkpoint = {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  validateLedgerOnlyIssueDisposition({
    label: "retained-checkpoint manual",
    selection: input.selection,
    disposition: "manual_required",
    checkpoint,
    expectedBaseSha: checkpoint.baseSha,
    parentLedgerMarkdown: input.parentLedgerMarkdown,
    pushedLedgerMarkdown: input.pushedLedgerMarkdown,
  });
};

/**
 * Native-review exhaustion is not a candidate delivery. Its only permitted
 * push is an atomic create of the immutable candidate ref plus a one-parent
 * docs-only development commit that leaves the issue open for a person.
 */
export const validateManualRequiredDevelopmentPush = (
  input: NativeManualLedgerPushValidationInput,
): void => {
  const update = selectDevelopmentPush(input.updates);
  if (!update) throw new Error("Sentinel manual-checkpoint push has no development update");
  const disposition = parseGitHubIssueManualRequiredReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = parseSentinelManualRequiredCycleReport(input.cycleValue, {
    runId: input.workflowRunId,
    runAttempt: input.workflowRunAttempt,
    status: "running",
    stage: "pushing_manual_required_github_issue",
    branchDispositions: ["runner_local_manual_atomic_push_in_flight"],
  });
  if (JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint)) {
    throw new Error("Sentinel manual-checkpoint push has no exact checkpoint receipt");
  }
  if (
    cycle.candidate_sha !== update.localSha || cycle.base_development_sha !== update.remoteSha
  ) {
    throw new Error("Sentinel manual-checkpoint push does not match the exact cycle commit");
  }
  if (input.commitParents.length !== 1 || input.commitParents[0] !== update.remoteSha) {
    throw new Error("Sentinel manual-checkpoint push must be a one-parent commit on the selected development base");
  }
  if (
    input.changedPaths.length !== 1 || input.changedPaths[0] !== SENTINEL_POLICY.paths.issueJobLedger
  ) {
    throw new Error("Sentinel manual-checkpoint push must change only the issue-job ledger");
  }
  if (!input.atomicPush || input.updates.length !== 2) {
    throw new Error("Sentinel manual-checkpoint push must be one atomic two-ref push");
  }
  const checkpointUpdate = selectManualCheckpointPush(input.updates, disposition.retry_checkpoint);
  if (checkpointUpdate.remoteSha !== ZERO_SHA || input.checkpointLeaseSha !== ZERO_SHA) {
    throw new Error("Sentinel manual-checkpoint push must create its candidate ref with a zero lease");
  }

  const expectedCheckpoint = {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  validateLedgerOnlyIssueDisposition({
    label: "manual-checkpoint",
    selection: input.selection,
    disposition: "manual_required",
    checkpoint: expectedCheckpoint,
    expectedBaseSha: expectedCheckpoint.baseSha,
    parentLedgerMarkdown: input.parentLedgerMarkdown,
    pushedLedgerMarkdown: input.pushedLedgerMarkdown,
  });
};

export const parseCandidateWorkflowValidationRecord = (
  value: unknown,
  expected: Readonly<{ candidateSha: string; candidateBranch: string }>,
): CandidateWorkflowValidationRecord => {
  const report = typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
  const correlationId = typeof report?.correlation_id === "string" ? report.correlation_id : "";
  const displayTitle = typeof report?.display_title === "string" ? report.display_title : "";
  if (
    !FULL_SHA.test(expected.candidateSha) || !SAFE_BRANCH.test(expected.candidateBranch) ||
    report?.schema_version !== 1 || report.source !== "build_only" ||
    report.git_sha !== expected.candidateSha || report.head_branch !== expected.candidateBranch ||
    !Number.isSafeInteger(report.workflow_run_id) || (report.workflow_run_id as number) <= 0 ||
    !SENTINEL_DEPLOYMENT_CORRELATION.test(correlationId) ||
    displayTitle !== `Deno Deploy ${correlationId}`
  ) {
    throw new Error("Sentinel candidate workflow validation does not match the exact candidate");
  }
  return {
    schema_version: 1,
    source: "build_only",
    git_sha: expected.candidateSha,
    head_branch: expected.candidateBranch,
    workflow_run_id: report.workflow_run_id as number,
    correlation_id: correlationId,
    display_title: displayTitle,
  };
};

const requireExactSuccessfulRun = (
  run: GitHubWorkflowRun,
  expected: Readonly<{ runId: number; candidateSha: string; displayTitle?: string }>,
): void => {
  if (run.id !== expected.runId) throw new Error("Sentinel candidate validation returned the wrong workflow run ID");
  if (run.headSha !== expected.candidateSha) {
    throw new Error(`Workflow run ${run.id} has the wrong candidate head SHA`);
  }
  if (expected.displayTitle !== undefined && run.displayTitle !== expected.displayTitle) {
    throw new Error(`Workflow run ${run.id} has the wrong candidate validation correlation`);
  }
  if (run.status !== "completed" || run.conclusion !== "success") {
    throw new Error(`Workflow run ${run.id} did not complete successfully`);
  }
};

export const ensureCandidateWorkflowValidation = async (
  input: Readonly<{
    client: CandidateWorkflowValidationClient;
    candidateSha: string;
    candidateBranch: string;
    disposition: "resolved" | "manual_required";
    preview: CandidatePreviewEvidence | null;
    existingBuildValidation: CandidateWorkflowValidationRecord | null;
    createCorrelationId?: () => string;
  }>,
): Promise<CandidateWorkflowValidationRecord> => {
  if (!FULL_SHA.test(input.candidateSha) || !SAFE_BRANCH.test(input.candidateBranch)) {
    throw new Error("Sentinel candidate workflow validation identity is invalid");
  }
  if (input.preview !== null) {
    if (input.preview.gitSha !== input.candidateSha) {
      throw new Error("Sentinel preview evidence has the wrong candidate SHA");
    }
    const run = await input.client.waitForWorkflow({
      runId: input.preview.workflowRunId,
      headSha: input.candidateSha,
    });
    requireExactSuccessfulRun(run, { runId: input.preview.workflowRunId, candidateSha: input.candidateSha });
    const correlationId = run.displayTitle.startsWith("Deno Deploy ")
      ? run.displayTitle.slice("Deno Deploy ".length)
      : "";
    if (!SENTINEL_DEPLOYMENT_CORRELATION.test(correlationId)) {
      throw new Error(`Workflow run ${run.id} has an invalid Sentinel deployment correlation`);
    }
    return {
      schema_version: 1,
      source: "preview",
      git_sha: input.candidateSha,
      head_branch: input.candidateBranch,
      workflow_run_id: run.id,
      correlation_id: correlationId,
      display_title: run.displayTitle,
    };
  }
  if (input.disposition === "resolved") {
    throw new Error("Resolved Sentinel issue candidates require exact preview workflow evidence");
  }
  if (input.existingBuildValidation !== null) {
    const evidence = input.existingBuildValidation;
    const run = await input.client.waitForWorkflow({
      runId: evidence.workflow_run_id,
      headSha: input.candidateSha,
      displayTitle: evidence.display_title,
    });
    requireExactSuccessfulRun(run, {
      runId: evidence.workflow_run_id,
      candidateSha: input.candidateSha,
      displayTitle: evidence.display_title,
    });
    return evidence;
  }

  const correlationId = input.createCorrelationId?.() ?? `sentinel-${crypto.randomUUID()}`;
  if (!SENTINEL_DEPLOYMENT_CORRELATION.test(correlationId)) {
    throw new Error("Sentinel candidate workflow validation correlation is invalid");
  }
  const displayTitle = `Deno Deploy ${correlationId}`;
  const dispatch = await input.client.dispatchWorkflow(
    SENTINEL_POLICY.github.deploymentWorkflow,
    input.candidateBranch,
    {
      deploy_preview: false,
      sentinel_build_only: true,
      sentinel_correlation_id: correlationId,
    },
  );
  const run = await input.client.waitForWorkflow({
    runId: dispatch.runId,
    headSha: input.candidateSha,
    displayTitle,
  });
  requireExactSuccessfulRun(run, { runId: dispatch.runId, candidateSha: input.candidateSha, displayTitle });
  return {
    schema_version: 1,
    source: "build_only",
    git_sha: input.candidateSha,
    head_branch: input.candidateBranch,
    workflow_run_id: dispatch.runId,
    correlation_id: correlationId,
    display_title: displayTitle,
  };
};

export const matchingIssueDeliveryPullRequests = (
  pulls: readonly PullRequest[],
  marker: string,
  candidateSha: string,
  candidateBranch: string,
): PullRequest[] =>
  pulls.filter((pull) =>
    pull.body.includes(marker) && pull.headSha === candidateSha && pull.headRef === candidateBranch &&
    pull.baseRef === "development"
  );

export const ensureIssuePullRequestForDevelopmentPush = async (
  input: Readonly<{
    repositoryRoot: string;
    prePushInput: string;
    token: string;
    repository: string;
    workflowRunId: string;
    workflowRunAttempt: number;
    serverUrl: string;
    atomicPush?: boolean;
    checkpointLeaseSha?: string | null;
  }>,
): Promise<GitHubIssuePullRequestRecord | null> => {
  const updates = parseGitPushUpdates(input.prePushInput);
  const update = selectDevelopmentPush(updates);
  if (!update) return null;
  const reportsDir = `${input.repositoryRoot}/.sentinel/reports`;
  let selectionValue: unknown;
  try {
    selectionValue = await readJson(`${reportsDir}/github-issue-selection.json`);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return null;
    throw error;
  }
  const selection = parseGitHubIssueSelectionReportAny(selectionValue);
  const cycleValue = await readJson(`${reportsDir}/cycle.json`);
  const cycle = parseSentinelCycleReport(cycleValue);
  if (selection.schema_version === 2) {
    // V2 delivery binding: the frozen plan must re-derive its exact digest,
    // its base must equal the authoritative prepared recovery base, and the
    // recovery source/run identity must bind the report before any PR action.
    const triageValue = await readJson(`${reportsDir}/triage.json`);
    const recovery = parseSentinelRecoveryRecord(await readJson(`${reportsDir}/recovery-record-v1.json`));
    await verifyFrozenIssuePlanDigest({
      repository: input.repository,
      selection,
      triageValue,
      cycleBaseSha: cycle.base_development_sha,
      repositoryRoot: input.repositoryRoot,
      recovery,
      runId: cycle.run_id,
    });
  }
  const dispositionValue = await readJson(`${reportsDir}/github-issue-disposition.json`);
  const dispositionRecord = typeof dispositionValue === "object" && dispositionValue !== null &&
      !Array.isArray(dispositionValue)
    ? dispositionValue as Record<string, unknown>
    : null;
  if (dispositionRecord?.disposition === "retry_pending") {
    parseGitHubIssueRetryPendingReport(dispositionValue, {
      issueId: selection.issue_id,
      issueNumber: selection.issue_number,
      fingerprint: selection.fingerprint,
    });
    const commitParts = (await git(["rev-list", "--parents", "-n", "1", update.localSha])).split(/\s+/u);
    const commitSha = commitParts.shift();
    if (commitSha !== update.localSha) {
      throw new Error("Sentinel retry-pending push returned the wrong commit identity");
    }
    const changedPaths = (await gitOutput([
      "diff-tree",
      "--no-commit-id",
      "--name-only",
      "-z",
      "-r",
      update.remoteSha,
      update.localSha,
      "--",
    ])).split("\0").filter((path) => path.length > 0);
    validateRetryPendingDevelopmentPush({
      workflowRunId: input.workflowRunId,
      updates,
      atomicPush: input.atomicPush ?? false,
      checkpointLeaseSha: input.checkpointLeaseSha ?? null,
      selection,
      cycleValue,
      dispositionValue,
      commitParents: commitParts,
      changedPaths,
      parentLedgerMarkdown: await gitOutput([
        "show",
        `${update.remoteSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
      pushedLedgerMarkdown: await gitOutput([
        "show",
        `${update.localSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
    });
    console.log(
      `[sentinel] issue_retry_pending=#${selection.issue_number} candidate=${update.localSha} delivery=none`,
    );
    return null;
  }
  if (
    dispositionRecord?.disposition === "manual_required" &&
    dispositionRecord.phase === "retry_checkpoint_resume_failed"
  ) {
    const commitParts = (await git(["rev-list", "--parents", "-n", "1", update.localSha])).split(/\s+/u);
    const commitSha = commitParts.shift();
    if (commitSha !== update.localSha) {
      throw new Error("Sentinel retained-checkpoint manual push returned the wrong commit identity");
    }
    const changedPaths = (await gitOutput([
      "diff-tree",
      "--no-commit-id",
      "--name-only",
      "-z",
      "-r",
      update.remoteSha,
      update.localSha,
      "--",
    ])).split("\0").filter((path) => path.length > 0);
    validateManualRequiredRetainedCheckpointDevelopmentPush({
      workflowRunId: input.workflowRunId,
      updates,
      atomicPush: input.atomicPush ?? false,
      checkpointLeaseSha: input.checkpointLeaseSha ?? null,
      selection,
      cycleValue,
      dispositionValue,
      commitParents: commitParts,
      changedPaths,
      parentLedgerMarkdown: await gitOutput([
        "show",
        `${update.remoteSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
      pushedLedgerMarkdown: await gitOutput([
        "show",
        `${update.localSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
    });
    console.log(
      `[sentinel] issue_manual_required=#${selection.issue_number} candidate=${update.localSha} delivery=none`,
    );
    return null;
  }
  if (
    dispositionRecord?.disposition === "manual_required" &&
    dispositionRecord.phase === "native_review_exhausted"
  ) {
    parseGitHubIssueManualRequiredReport(dispositionValue, {
      issueId: selection.issue_id,
      issueNumber: selection.issue_number,
      fingerprint: selection.fingerprint,
    });
    const commitParts = (await git(["rev-list", "--parents", "-n", "1", update.localSha])).split(/\s+/u);
    const commitSha = commitParts.shift();
    if (commitSha !== update.localSha) {
      throw new Error("Sentinel manual-checkpoint push returned the wrong commit identity");
    }
    const changedPaths = (await gitOutput([
      "diff-tree",
      "--no-commit-id",
      "--name-only",
      "-z",
      "-r",
      update.remoteSha,
      update.localSha,
      "--",
    ])).split("\0").filter((path) => path.length > 0);
    validateManualRequiredDevelopmentPush({
      workflowRunId: input.workflowRunId,
      workflowRunAttempt: input.workflowRunAttempt,
      updates,
      atomicPush: input.atomicPush ?? false,
      checkpointLeaseSha: input.checkpointLeaseSha ?? null,
      selection,
      cycleValue,
      dispositionValue,
      commitParents: commitParts,
      changedPaths,
      parentLedgerMarkdown: await gitOutput([
        "show",
        `${update.remoteSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
      pushedLedgerMarkdown: await gitOutput([
        "show",
        `${update.localSha}:${SENTINEL_POLICY.paths.issueJobLedger}`,
      ]),
    });
    console.log(
      `[sentinel] issue_manual_required=#${selection.issue_number} candidate=${update.localSha} delivery=none`,
    );
    return null;
  }
  if (!cycle.temporary_branch || !cycle.temporary_branch.startsWith("sentinel/candidate-")) {
    throw new Error("Sentinel issue delivery has no valid candidate branch");
  }
  if (cycle.candidate_sha !== update.localSha) {
    const pullValue = await readJson(`${reportsDir}/github-issue-pull-request.json`);
    const pullRecord = parseGitHubIssuePullRequestRecord(pullValue);
    const parentSha = await git(["rev-parse", `${update.localSha}^`]);
    if (
      !isIssueDeliveryFailSafeRevert({
        selection,
        cycle,
        pullRequest: pullRecord,
        pushedSha: update.localSha,
        parentSha,
      })
    ) {
      throw new Error("Sentinel development push does not match the issue candidate or its fail-safe revert");
    }
    return pullRecord;
  }
  await ensureRemoteCandidateBranch(cycle.temporary_branch, update.localSha);

  const marker = issuePullRequestMarker(selection);
  const existing = matchingIssueDeliveryPullRequests(
    await listPullRequests(input.token, input.repository),
    marker,
    update.localSha,
    cycle.temporary_branch,
  );
  if (existing.length > 1) {
    throw new Error("More than one pull request exists for the concrete Sentinel issue delivery attempt");
  }

  const disposition = parseIssueCandidateDisposition(
    dispositionValue,
    { issueNumber: selection.issue_number, fingerprint: selection.fingerprint },
  );
  const preview = await optionalPreviewEvidence(reportsDir, update.localSha);
  const workflowRunUrl = `${input.serverUrl}/${input.repository}/actions/runs/${input.workflowRunId}`;
  const body = renderIssuePullRequestBody({
    repository: input.repository,
    selection,
    cycle,
    candidateSha: update.localSha,
    workflowRunUrl,
    validationReports: await reportNames(reportsDir, /^validation-(?:round-[1-3]|manual-github-issue)\.json$/u),
    nativeReviewReports: await reportNames(reportsDir, /^native-review-round-[1-3]\.json$/u),
    replayReports: await reportNames(reportsDir, /^replay-round-[1-3]\.json$/u),
    previewRevision: preview?.revision ?? null,
    previewWorkflowRunId: preview?.workflowRunId ?? null,
  });

  let pull: PullRequest;
  let reused = false;
  if (existing.length === 1) {
    pull = existing[0]!;
    reused = true;
    if (pull.state !== "open" || pull.mergedAt !== null) {
      throw new Error("Existing Sentinel delivery-attempt pull request is no longer open");
    }
    pull = parsePullRequest(
      await githubRequest(input.token, input.repository, `/pulls/${pull.number}`, {
        method: "PATCH",
        body: JSON.stringify({ body }),
      }),
    );
  } else {
    pull = parsePullRequest(
      await githubRequest(input.token, input.repository, "/pulls", {
        method: "POST",
        body: JSON.stringify({
          title: `fix(issue #${selection.issue_number}): Provider Sentinel deliverable`,
          body,
          head: cycle.temporary_branch,
          base: "development",
          draft: false,
          maintainer_can_modify: false,
        }),
      }),
    );
  }
  if (
    pull.state !== "open" || pull.mergedAt !== null || pull.headRef !== cycle.temporary_branch ||
    pull.headSha !== update.localSha || pull.baseRef !== "development" || !pull.body.includes(marker)
  ) {
    throw new Error("Sentinel issue pull request failed its post-creation identity check");
  }

  const record: GitHubIssuePullRequestRecord = {
    schema_version: 1,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    pull_request_number: pull.number,
    pull_request_url: pull.htmlUrl,
    head_branch: pull.headRef,
    head_sha: pull.headSha,
    base_branch: "development",
    marker,
    reused,
  };
  await Deno.writeTextFile(
    `${reportsDir}/github-issue-pull-request.json`,
    `${JSON.stringify(record, null, 2)}\n`,
    { mode: 0o600 },
  );

  const existingValidationValue = disposition === "manual_required" && preview === null
    ? await optionalJson(`${reportsDir}/candidate-workflow-validation.json`)
    : null;
  const existingBuildValidation = existingValidationValue === null
    ? null
    : parseCandidateWorkflowValidationRecord(existingValidationValue, {
      candidateSha: update.localSha,
      candidateBranch: cycle.temporary_branch,
    });
  const validation = await ensureCandidateWorkflowValidation({
    client: new GitHubActionsClient({ repository: input.repository, token: input.token }),
    candidateSha: update.localSha,
    candidateBranch: cycle.temporary_branch,
    disposition,
    preview,
    existingBuildValidation,
  });
  await Deno.writeTextFile(
    `${reportsDir}/candidate-workflow-validation.json`,
    `${JSON.stringify(validation, null, 2)}\n`,
    { mode: 0o600 },
  );
  console.log(
    `[sentinel] issue_pull_request=#${pull.number} issue=#${selection.issue_number} reused=${reused} ` +
      `validation_run=${validation.workflow_run_id} validation_source=${validation.source}`,
  );
  return record;
};

if (import.meta.main) {
  const repositoryRoot = await Deno.realPath(requiredEnvironment("GITHUB_WORKSPACE"));
  const prePushInput = await new Response(Deno.stdin.readable).text();
  const workflowRunAttempt = Number(requiredEnvironment("GITHUB_RUN_ATTEMPT"));
  if (!Number.isSafeInteger(workflowRunAttempt) || workflowRunAttempt <= 0) {
    throw new Error("GITHUB_RUN_ATTEMPT must be a positive integer for Sentinel issue delivery");
  }
  await ensureIssuePullRequestForDevelopmentPush({
    repositoryRoot,
    prePushInput,
    token: requiredEnvironment("GITHUB_TOKEN"),
    repository: requiredEnvironment("GITHUB_REPOSITORY"),
    workflowRunId: requiredEnvironment("GITHUB_RUN_ID"),
    workflowRunAttempt,
    serverUrl: Deno.env.get("GITHUB_SERVER_URL")?.trim() || "https://github.com",
    atomicPush: Deno.env.get("SENTINEL_GIT_PUSH_ATOMIC") === "1",
    checkpointLeaseSha: Deno.env.get("SENTINEL_GIT_CHECKPOINT_LEASE_SHA")?.trim() || null,
  });
}
