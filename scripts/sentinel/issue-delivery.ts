import {
  isSentinelRecoveryCandidateBranch,
  sentinelRecoveryCandidateBranch,
  type SentinelRecoveryIdentityV1,
  type SentinelRecoveryRecordV1,
} from "./recovery.ts";
import { canonicalMatrixJson } from "./matrix.ts";
import { isTriageReport, type TriageReport } from "./types.ts";
import { SENTINEL_POLICY } from "./policy.ts";
import { parseGitHubIssueJobLedger } from "./issues.ts";
import {
  MAX_ROLLING_REVIEW_RESULT_BYTES,
  parseRollingReviewResult,
  parseRollingReviewResultFileNames,
} from "./rolling-review.ts";
import { runTrustedGit, runTrustedGitUnchecked } from "./validation.ts";

export { isSentinelRecoveryCandidateBranch, sentinelRecoveryCandidateBranch };

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const CHECKPOINT_BRANCH =
  /^sentinel\/candidate-(?:[1-9][0-9]*(?:-[1-9][0-9]*)?|(?:github_issue|review_backlog|triage|incident)-[A-Za-z0-9][A-Za-z0-9._-]{0,79}-[A-Za-z0-9][A-Za-z0-9._-]{0,31}-g[1-9][0-9]*-[0-9a-f]{16})$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const SAFE_URL = /^https:\/\/github\.com\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\/(?:pull|actions\/runs)\/[1-9][0-9]*$/u;
const WORKFLOW_RUN_ID = /^[1-9][0-9]*$/u;

/** A collision-resistant, serialisable key for the immutable recovery identity. */
export const sentinelRecoveryIdentityKey = (identity: SentinelRecoveryIdentityV1): string => {
  sentinelRecoveryCandidateBranch(identity);
  return JSON.stringify([
    identity.repository,
    identity.source_kind,
    identity.source_id,
    identity.source_revision,
    identity.candidate_generation,
  ]);
};

const currentWorkflowCandidateBranch = (branch: string, runId: string, runAttempt: number): boolean =>
  CHECKPOINT_BRANCH.test(branch) && branch === `sentinel/candidate-${runId}-${runAttempt}`;

export const ISSUE_COMPLETION_EVIDENCE_TEXT =
  "Delivered, merged, and verified in production; issue closed as completed.";

const safeSelectionPath = (path: unknown): path is string =>
  typeof path === "string" && path.length > 0 && path.length <= 512 && !path.startsWith("/") &&
  !path.includes("\\") && !path.split("/").some((part) => part === "" || part === "." || part === "..");

export type GitHubIssueSelectionReport = Readonly<{
  schema_version: 1 | 2;
  issue_id: number;
  issue_number: number;
  fingerprint: string;
  body_sha256: string;
  comments: number;
  priority: "P2" | "P3";
  time_label: string | null;
  files: readonly string[];
  updated_at: string;
  /** V2: exact development base the frozen plan was prepared against. */
  base_sha?: string;
  /** V2: digest binding repository, issue identity, source fingerprint, base SHA, and canonical frozen triage. */
  plan_sha256?: string;
}>;

/**
 * The canonical frozen-plan digest for a V2 selection report: repository,
 * issue identity, source fingerprint, exact base SHA, and the canonical full
 * TriageReport (scope, validation requirements, evidence).
 */
export const githubIssuePlanDigest = async (
  input: Readonly<{
    repository: string;
    issue_id: number;
    fingerprint: string;
    base_sha: string;
    plan: TriageReport;
  }>,
): Promise<string> => {
  if (
    !SAFE_REPOSITORY.test(input.repository) || !positiveInteger(input.issue_id) ||
    !SHA256.test(input.fingerprint) || !FULL_SHA.test(input.base_sha)
  ) {
    throw new Error("Sentinel GitHub issue plan digest input is invalid");
  }
  const canonical = canonicalMatrixJson({
    repository: input.repository,
    issue_id: input.issue_id,
    fingerprint: input.fingerprint,
    base_sha: input.base_sha,
    plan: input.plan,
  });
  return await sha256Hex(canonical);
};

const textEncoder = new TextEncoder();
const sha256Hex = async (value: string): Promise<string> =>
  [...new Uint8Array(await crypto.subtle.digest("SHA-256", textEncoder.encode(value)))]
    .map((byte) => byte.toString(16).padStart(2, "0")).join("");

export type SentinelCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  candidate_sha: string | null;
  temporary_branch: string | null;
  status: string;
  stage: string;
  evidence_artifact_name: string | null;
  /** The pinned development base the cycle prepared against (V2 binding). */
  base_development_sha: string | null;
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

export type GitHubIssueManualRequiredRetainedCheckpointReport = Readonly<{
  schema_version: 1;
  issue_id: number;
  issue_number: number;
  fingerprint: string;
  phase: "retry_checkpoint_resume_failed";
  implementation_status: "blocked";
  disposition: "manual_required";
  retry_checkpoint: GitHubIssueRetryCheckpointReport;
}>;

export type GitHubIssueRetryCheckpointReport = Readonly<{
  branch: string;
  sha: string;
  base_sha: string;
}>;

/**
 * A manual checkpoint preserves an unsafe candidate for a human. It has the
 * same immutable Git identity shape as a retry checkpoint, but it is never a
 * signal to resume or redeliver the candidate automatically.
 */
export type GitHubIssueManualCheckpointReport = GitHubIssueRetryCheckpointReport;

export type GitHubIssueManualRequiredReport = Readonly<{
  schema_version: 1;
  issue_id: number;
  issue_number: number;
  fingerprint: string;
  phase: "native_review_exhausted";
  implementation_status: "blocked";
  disposition: "manual_required";
  retry_checkpoint: GitHubIssueManualCheckpointReport;
}>;

export type SentinelRetryPendingCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  started_at: string;
  base_development_sha: string;
  candidate_sha: string;
  temporary_branch: string;
  status: "running" | "no_change" | "failed";
  stage: "pushing_retry_pending_github_issue" | "complete" | "failed";
  branch_disposition:
    | "runner_local_pending_review"
    | "runner_local_atomic_push_in_flight"
    | "development_docs_only_issue_retry_pending"
    | "remote_retained_issue_retry_pending"
    | "remote_retained_atomic_push_in_flight"
    | "atomic_retry_push_requires_reconciliation";
  retry_checkpoint: GitHubIssueRetryCheckpointReport | null;
}>;

export type SentinelManualRequiredRetainedCheckpointCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  started_at: string;
  base_development_sha: string;
  candidate_sha: string;
  temporary_branch: string;
  status: "running" | "no_change";
  stage: "pushing_manual_github_issue" | "complete";
  branch_disposition: "runner_local_pending_review" | "development_docs_only_issue_manual_required";
  retry_checkpoint: GitHubIssueRetryCheckpointReport;
}>;

export type SentinelManualRequiredCycleReport = Readonly<{
  schema_version: 1;
  run_id: string;
  started_at: string;
  base_development_sha: string;
  candidate_sha: string;
  temporary_branch: string;
  status: "running" | "no_change" | "failed";
  stage: "pushing_manual_required_github_issue" | "complete" | "failed";
  branch_disposition:
    | "runner_local_manual_atomic_push_in_flight"
    | "remote_retained_manual_atomic_push_in_flight"
    | "atomic_manual_push_accepted_unverified"
    | "remote_retained_issue_manual_required"
    | "atomic_manual_push_requires_reconciliation";
  retry_checkpoint: GitHubIssueManualCheckpointReport;
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

const parseCheckpoint = (
  value: unknown,
  label: "manual" | "retry",
): GitHubIssueRetryCheckpointReport | null => {
  if (value === null) return null;
  const checkpoint = record(value);
  if (
    !checkpoint || Object.keys(checkpoint).length !== 3 ||
    typeof checkpoint.branch !== "string" || !CHECKPOINT_BRANCH.test(checkpoint.branch) ||
    typeof checkpoint.sha !== "string" || !FULL_SHA.test(checkpoint.sha) ||
    typeof checkpoint.base_sha !== "string" || !FULL_SHA.test(checkpoint.base_sha) ||
    checkpoint.sha === checkpoint.base_sha
  ) throw new Error(`Sentinel ${label} checkpoint report is invalid`);
  return { branch: checkpoint.branch, sha: checkpoint.sha, base_sha: checkpoint.base_sha };
};

const parseRetryCheckpoint = (value: unknown): GitHubIssueRetryCheckpointReport | null =>
  parseCheckpoint(value, "retry");

const parseManualCheckpoint = (value: unknown): GitHubIssueManualCheckpointReport | null =>
  parseCheckpoint(value, "manual");

/**
 * Bounded linear advance allowed between the original frozen plan base and the
 * current execution base: the same eight-commit review advance limit used by
 * the matrix convergence advance verifier.
 */
export const SENTINEL_PLAN_BASE_ADVANCE_COMMIT_LIMIT = 8 as const;

const regularBlobTreeMetadata = async (
  repositoryRoot: string,
  revision: string,
  path: string,
): Promise<Readonly<{ mode: string; objectType: string; sha: string; path: string }>> => {
  const tree = new TextDecoder("utf-8", { fatal: true }).decode(
    (await runTrustedGit({
      args: ["ls-tree", "-z", revision, "--", path],
      cwd: repositoryRoot,
    })).stdout,
  ).split("\0").filter((entry) => entry.length > 0);
  const treeEntry = tree.length === 1 ? tree[0]! : null;
  const separator = treeEntry?.indexOf("\t") ?? -1;
  const metadata = separator >= 0 ? treeEntry!.slice(0, separator).split(" ") : [];
  return {
    mode: metadata.length === 3 ? metadata[0]! : "",
    objectType: metadata.length === 3 ? metadata[1]! : "",
    sha: metadata.length === 3 ? metadata[2]! : "",
    path: separator >= 0 ? treeEntry!.slice(separator + 1) : "",
  };
};

const readPathText = async (repositoryRoot: string, revision: string, path: string): Promise<string> => {
  const output = await runTrustedGit({
    args: ["show", `${revision}:${path}`],
    cwd: repositoryRoot,
    maximumOutputBytes: MAX_ROLLING_REVIEW_RESULT_BYTES + 512 * 1_024,
  });
  return new TextDecoder("utf-8", { fatal: true }).decode(output.stdout);
};

/**
 * Trusted base-compatibility proof for a V2 frozen plan carried across runs:
 * the original plan base must either equal the current execution base, or be
 * its ancestor through a bounded linear single-parent history in which every
 * intervening commit is either a canonical issue-job ledger-only update or an
 * exactly one fully validated rolling review result add. Source-code changes,
 * merges, deletes, renames, rewrites, invalid ledger/review data and unproved
 * ancestry are rejected; an actual repository checkout is always required.
 */
export const verifyIssuePlanBaseCompatibility = async (
  input: Readonly<{
    repositoryRoot: string;
    planBaseSha: string;
    executionBaseSha: string;
  }>,
): Promise<void> => {
  const { repositoryRoot, planBaseSha, executionBaseSha } = input;
  if (!FULL_SHA.test(planBaseSha)) throw new Error("Sentinel frozen plan base SHA is invalid");
  if (!FULL_SHA.test(executionBaseSha)) throw new Error("Sentinel frozen plan execution base SHA is invalid");
  if (planBaseSha === executionBaseSha) return;
  const ancestry = await runTrustedGitUnchecked({
    args: ["merge-base", "--is-ancestor", planBaseSha, executionBaseSha],
    cwd: repositoryRoot,
  });
  if (ancestry.code !== 0) {
    throw new Error("Sentinel frozen plan base is not an ancestor of the execution base");
  }
  const commitCountText = new TextDecoder("utf-8", { fatal: true }).decode(
    (await runTrustedGit({
      args: ["rev-list", "--count", `${planBaseSha}..${executionBaseSha}`],
      cwd: repositoryRoot,
    })).stdout,
  ).trim();
  const commitCount = Number(commitCountText);
  if (
    !Number.isSafeInteger(commitCount) || commitCount < 1 ||
    commitCount > SENTINEL_PLAN_BASE_ADVANCE_COMMIT_LIMIT
  ) {
    throw new Error("Sentinel frozen plan base advance exceeds the allowed commit count");
  }
  const commits = new TextDecoder("utf-8", { fatal: true }).decode(
    (await runTrustedGit({
      args: ["rev-list", "--reverse", "--topo-order", `${planBaseSha}..${executionBaseSha}`],
      cwd: repositoryRoot,
    })).stdout,
  ).split("\n").filter((line) => line.length > 0);
  if (commits.length !== commitCount) {
    throw new Error("Sentinel frozen plan base advance commit listing is inconsistent");
  }
  const reviewResultsPrefix = `${SENTINEL_POLICY.paths.reviewResults}/`;
  for (const [index, commit] of commits.entries()) {
    const parents = new TextDecoder("utf-8", { fatal: true }).decode(
      (await runTrustedGit({ args: ["show", "-s", "--format=%P", commit], cwd: repositoryRoot })).stdout,
    ).trim().split(" ").filter((value) => value.length > 0);
    if (parents.length !== 1) {
      throw new Error(`Sentinel frozen plan base advance commit ${commit} is not a single-parent commit`);
    }
    const expectedParent = index === 0 ? planBaseSha : commits[index - 1]!;
    if (parents[0] !== expectedParent) {
      throw new Error(`Sentinel frozen plan base advance commit ${commit} is not on the planned linear path`);
    }
    const changed = new TextDecoder("utf-8", { fatal: true }).decode(
      (await runTrustedGit({
        args: [
          "diff-tree",
          "--no-commit-id",
          "--no-ext-diff",
          "--no-textconv",
          "--name-status",
          "--no-renames",
          "-r",
          "-z",
          commit,
        ],
        cwd: repositoryRoot,
      })).stdout,
    ).split("\0").filter((entry) => entry.length > 0);
    if (changed.length !== 2) {
      throw new Error(`Sentinel frozen plan base advance commit ${commit} changes multiple paths`);
    }
    const [status, path] = changed;
    if (status === "M" && path === SENTINEL_POLICY.paths.issueJobLedger) {
      const [before, after] = await Promise.all([
        regularBlobTreeMetadata(repositoryRoot, expectedParent, path),
        regularBlobTreeMetadata(repositoryRoot, commit, path),
      ]);
      if (
        before.mode !== "100644" || before.objectType !== "blob" || !FULL_SHA.test(before.sha) ||
        before.path !== path || after.mode !== "100644" || after.objectType !== "blob" ||
        !FULL_SHA.test(after.sha) || after.path !== path
      ) {
        throw new Error(`Sentinel frozen plan base advance ledger is not a regular 100644 Git file: ${path}`);
      }
      // Both revisions must be canonical complete valid issue-job ledgers.
      parseGitHubIssueJobLedger(await readPathText(repositoryRoot, expectedParent, path));
      parseGitHubIssueJobLedger(await readPathText(repositoryRoot, commit, path));
      continue;
    }
    if (status === "A" && path.startsWith(reviewResultsPrefix)) {
      const fileName = path.slice(reviewResultsPrefix.length);
      const identities = parseRollingReviewResultFileNames([path]);
      if (identities.length !== 1) {
        throw new Error(`Sentinel frozen plan base advance result file name is invalid: ${fileName}`);
      }
      const identity = identities[0]!;
      const treeEntry = await regularBlobTreeMetadata(repositoryRoot, commit, path);
      if (
        treeEntry.mode !== "100644" || treeEntry.objectType !== "blob" ||
        !FULL_SHA.test(treeEntry.sha) || treeEntry.path !== path
      ) {
        throw new Error(`Sentinel frozen plan base advance result path is not a regular 100644 Git blob: ${path}`);
      }
      const blob = await runTrustedGit({
        args: ["show", `${commit}:${path}`],
        cwd: repositoryRoot,
        maximumOutputBytes: MAX_ROLLING_REVIEW_RESULT_BYTES + 64 * 1_024,
      });
      if (blob.stdout.byteLength > MAX_ROLLING_REVIEW_RESULT_BYTES) {
        throw new Error(`Sentinel frozen plan base advance result blob exceeds its byte limit: ${path}`);
      }
      const text = new TextDecoder("utf-8", { fatal: true }).decode(blob.stdout);
      let value: unknown;
      try {
        value = JSON.parse(text);
      } catch {
        throw new Error(`Sentinel frozen plan base advance result blob is not valid JSON: ${path}`);
      }
      parseRollingReviewResult(value, { prNumber: identity.prNumber, headSha: identity.headSha, fileName });
      continue;
    }
    throw new Error(`Sentinel frozen plan base advance commit ${commit} changes a disallowed path`);
  }
};

/**
 * Fail-closed verification of a V2 frozen plan at delivery/closure callers:
 * the frozen triage at the run reports path must re-derive the exact bound
 * plan digest for the same source identity. V1 stays legacy (no re-binding).
 */
export const verifyFrozenIssuePlanDigest = async (
  input: Readonly<{
    repository: string;
    selection: GitHubIssueSelectionReport;
    triageValue: unknown;
    cycleBaseSha: string | null;
    repositoryRoot?: string;
    recovery?: SentinelRecoveryRecordV1;
    runId?: string;
  }>,
): Promise<void> => {
  if (input.selection.schema_version !== 2) return;
  if (!isTriageReport(input.triageValue)) throw new Error("Sentinel frozen triage report is invalid");
  if (input.cycleBaseSha !== null && input.selection.base_sha !== input.cycleBaseSha) {
    if (input.repositoryRoot === undefined) {
      throw new Error("Sentinel frozen plan base does not match the cycle development base");
    }
    // The execution base may only advance from the ORIGINAL plan base through
    // trusted bounded linear history: ledger-only or validated review results.
    await verifyIssuePlanBaseCompatibility({
      repositoryRoot: input.repositoryRoot,
      planBaseSha: input.selection.base_sha!,
      executionBaseSha: input.cycleBaseSha,
    });
  }
  if (input.recovery !== undefined) {
    const identity = input.recovery.identity;
    if (
      identity.repository !== input.repository || identity.source_kind !== "github_issue" ||
      identity.source_id !== String(input.selection.issue_id) ||
      identity.source_revision !== input.selection.fingerprint
    ) {
      throw new Error("Sentinel frozen plan does not bind the authoritative recovery source identity");
    }
    if (input.recovery.base_sha !== input.selection.base_sha) {
      // The recovery record carries the current execution base while the
      // ORIGINAL plan base is preserved; the advance must prove compatible.
      if (input.repositoryRoot === undefined) {
        throw new Error("Sentinel frozen plan does not bind the authoritative recovery execution base");
      }
      await verifyIssuePlanBaseCompatibility({
        repositoryRoot: input.repositoryRoot,
        planBaseSha: input.selection.base_sha!,
        executionBaseSha: input.recovery.base_sha,
      });
    }
    if (input.runId !== undefined && input.recovery.run_id !== input.runId) {
      throw new Error("Sentinel frozen plan recovery run does not match the delivery run");
    }
  }
  const digest = await githubIssuePlanDigest({
    repository: input.repository,
    issue_id: input.selection.issue_id,
    fingerprint: input.selection.fingerprint,
    base_sha: input.selection.base_sha!,
    plan: input.triageValue,
  });
  if (digest !== input.selection.plan_sha256) {
    throw new Error("Sentinel frozen plan digest does not match the frozen triage");
  }
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
    !selection.files.every(safeSelectionPath) || !isoTimestamp(selection.updated_at)
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

/**
 * Universal V1/V2 selection report reader used by every report consumer: V1
 * is strictly legacy, V2 adds base/plan binding.
 */
export const parseGitHubIssueSelectionReportAny = (value: unknown): GitHubIssueSelectionReport => {
  const selection = record(value);
  if (selection === null) throw new Error("Sentinel GitHub issue selection report is invalid");
  return selection.schema_version === 2
    ? parseGitHubIssueSelectionReportV2(selection)
    : parseGitHubIssueSelectionReport(selection);
};

/**
 * Strict V2 frozen-plan selection report reader. V2 keeps the source fields
 * (nullable estimate, empty source hints allowed) and binds the exact
 * development base and the canonical frozen triage digest.
 */
export const parseGitHubIssueSelectionReportV2 = (value: unknown): GitHubIssueSelectionReport => {
  const selection = record(value);
  if (
    !selection || selection.schema_version !== 2 || !positiveInteger(selection.issue_id) ||
    !positiveInteger(selection.issue_number) || typeof selection.fingerprint !== "string" ||
    !SHA256.test(selection.fingerprint) || typeof selection.body_sha256 !== "string" ||
    !SHA256.test(selection.body_sha256) || !nonNegativeInteger(selection.comments) ||
    (selection.priority !== "P2" && selection.priority !== "P3") ||
    !(selection.time_label === null ||
      (typeof selection.time_label === "string" && selection.time_label.length > 0 &&
        selection.time_label.length <= 80)) ||
    !Array.isArray(selection.files) || selection.files.length > 32 ||
    !selection.files.every(safeSelectionPath) || !isoTimestamp(selection.updated_at) ||
    typeof selection.base_sha !== "string" || !FULL_SHA.test(selection.base_sha) ||
    typeof selection.plan_sha256 !== "string" || !SHA256.test(selection.plan_sha256)
  ) throw new Error("Sentinel GitHub issue selection V2 report is invalid");
  return {
    schema_version: 2,
    issue_id: selection.issue_id,
    issue_number: selection.issue_number,
    fingerprint: selection.fingerprint,
    body_sha256: selection.body_sha256,
    comments: selection.comments,
    priority: selection.priority,
    time_label: selection.time_label,
    files: [...selection.files] as string[],
    updated_at: selection.updated_at,
    base_sha: selection.base_sha,
    plan_sha256: selection.plan_sha256,
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

export const parseGitHubIssueManualRequiredRetainedCheckpointReport = (
  value: unknown,
  expected: Readonly<{ issueId: number; issueNumber: number; fingerprint: string }>,
): GitHubIssueManualRequiredRetainedCheckpointReport => {
  const report = record(value);
  if (
    !positiveInteger(expected.issueId) || !positiveInteger(expected.issueNumber) ||
    !SHA256.test(expected.fingerprint) || report?.schema_version !== 1 ||
    report.issue_id !== expected.issueId || report.issue_number !== expected.issueNumber ||
    report.fingerprint !== expected.fingerprint || report.phase !== "retry_checkpoint_resume_failed" ||
    report.implementation_status !== "blocked" || report.disposition !== "manual_required"
  ) {
    throw new Error("Sentinel retained-checkpoint manual disposition does not match the exact issue selection");
  }
  const retryCheckpoint = parseRetryCheckpoint(report.retry_checkpoint);
  if (retryCheckpoint === null) {
    throw new Error("Sentinel retained-checkpoint manual disposition has no durable retry checkpoint");
  }
  return {
    schema_version: 1,
    issue_id: expected.issueId,
    issue_number: expected.issueNumber,
    fingerprint: expected.fingerprint,
    phase: "retry_checkpoint_resume_failed",
    implementation_status: "blocked",
    disposition: "manual_required",
    retry_checkpoint: retryCheckpoint,
  };
};

export const parseGitHubIssueManualRequiredReport = (
  value: unknown,
  expected: Readonly<{ issueId: number; issueNumber: number; fingerprint: string }>,
): GitHubIssueManualRequiredReport => {
  const report = record(value);
  const expectedKeys = [
    "schema_version",
    "issue_id",
    "issue_number",
    "fingerprint",
    "phase",
    "implementation_status",
    "disposition",
    "retry_checkpoint",
  ].sort();
  if (
    !positiveInteger(expected.issueId) || !positiveInteger(expected.issueNumber) ||
    !SHA256.test(expected.fingerprint) || !report ||
    JSON.stringify(Object.keys(report).sort()) !== JSON.stringify(expectedKeys) ||
    report.schema_version !== 1 || report.issue_id !== expected.issueId ||
    report.issue_number !== expected.issueNumber || report.fingerprint !== expected.fingerprint ||
    report.phase !== "native_review_exhausted" || report.implementation_status !== "blocked" ||
    report.disposition !== "manual_required"
  ) {
    throw new Error("Sentinel manual-checkpoint disposition does not match the exact issue selection");
  }
  const checkpoint = parseManualCheckpoint(report.retry_checkpoint);
  if (checkpoint === null) {
    throw new Error("Sentinel manual-checkpoint disposition requires an immutable candidate");
  }
  return {
    schema_version: 1,
    issue_id: expected.issueId,
    issue_number: expected.issueNumber,
    fingerprint: expected.fingerprint,
    phase: "native_review_exhausted",
    implementation_status: "blocked",
    disposition: "manual_required",
    retry_checkpoint: checkpoint,
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
    retryCheckpoint === null
      ? cycle.branch_disposition === "remote_retained_issue_retry_pending" ||
        cycle.branch_disposition === "remote_retained_atomic_push_in_flight" ||
        cycle.branch_disposition === "atomic_retry_push_requires_reconciliation"
      : cycle.branch_disposition !== "runner_local_pending_review" &&
        cycle.branch_disposition !== "runner_local_atomic_push_in_flight" &&
        cycle.branch_disposition !== "remote_retained_issue_retry_pending" &&
        cycle.branch_disposition !== "remote_retained_atomic_push_in_flight" &&
        cycle.branch_disposition !== "atomic_retry_push_requires_reconciliation"
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

export const parseSentinelManualRequiredRetainedCheckpointCycleReport = (
  value: unknown,
  expected: Readonly<{
    runId: string;
    status: SentinelManualRequiredRetainedCheckpointCycleReport["status"];
    stage: SentinelManualRequiredRetainedCheckpointCycleReport["stage"];
    branchDisposition: SentinelManualRequiredRetainedCheckpointCycleReport["branch_disposition"];
  }>,
): SentinelManualRequiredRetainedCheckpointCycleReport => {
  const cycle = record(value);
  if (
    expected.runId.length === 0 || expected.runId.length > 200 ||
    !cycle || cycle.schema_version !== 1 || cycle.run_id !== expected.runId ||
    typeof cycle.started_at !== "string" || !isoTimestamp(cycle.started_at) ||
    typeof cycle.base_development_sha !== "string" || !FULL_SHA.test(cycle.base_development_sha) ||
    typeof cycle.candidate_sha !== "string" || !FULL_SHA.test(cycle.candidate_sha) ||
    cycle.candidate_sha === cycle.base_development_sha ||
    typeof cycle.temporary_branch !== "string" || !CHECKPOINT_BRANCH.test(cycle.temporary_branch) ||
    cycle.status !== expected.status || cycle.stage !== expected.stage ||
    cycle.branch_disposition !== expected.branchDisposition
  ) {
    throw new Error("Sentinel retained-checkpoint manual cycle report is invalid");
  }
  const retryCheckpoint = parseRetryCheckpoint(cycle.retry_checkpoint);
  if (retryCheckpoint === null || retryCheckpoint.branch === cycle.temporary_branch) {
    throw new Error("Sentinel retained-checkpoint manual cycle is not bound to a prior attempt");
  }
  return {
    schema_version: 1,
    run_id: expected.runId,
    started_at: cycle.started_at,
    base_development_sha: cycle.base_development_sha,
    candidate_sha: cycle.candidate_sha,
    temporary_branch: cycle.temporary_branch,
    status: expected.status,
    stage: expected.stage,
    branch_disposition: expected.branchDisposition,
    retry_checkpoint: retryCheckpoint,
  };
};

export const parseSentinelManualRequiredCycleReport = (
  value: unknown,
  expected: Readonly<{
    runId: string;
    runAttempt: number;
    stage: SentinelManualRequiredCycleReport["stage"];
    status: SentinelManualRequiredCycleReport["status"];
    branchDispositions: readonly SentinelManualRequiredCycleReport["branch_disposition"][];
  }>,
): SentinelManualRequiredCycleReport => {
  const cycle = record(value);
  if (
    !WORKFLOW_RUN_ID.test(expected.runId) || !positiveInteger(expected.runAttempt) ||
    !cycle || cycle.schema_version !== 1 || cycle.run_id !== expected.runId ||
    typeof cycle.started_at !== "string" || !isoTimestamp(cycle.started_at) ||
    typeof cycle.base_development_sha !== "string" || !FULL_SHA.test(cycle.base_development_sha) ||
    typeof cycle.candidate_sha !== "string" || !FULL_SHA.test(cycle.candidate_sha) ||
    typeof cycle.temporary_branch !== "string" ||
    !currentWorkflowCandidateBranch(cycle.temporary_branch, expected.runId, expected.runAttempt) ||
    cycle.status !== expected.status || cycle.stage !== expected.stage ||
    expected.branchDispositions.length === 0 ||
    !expected.branchDispositions.includes(
      cycle.branch_disposition as SentinelManualRequiredCycleReport["branch_disposition"],
    )
  ) {
    throw new Error("Sentinel manual-checkpoint cycle report is invalid");
  }
  const checkpoint = parseManualCheckpoint(cycle.retry_checkpoint);
  if (
    checkpoint === null || checkpoint.branch !== cycle.temporary_branch ||
    checkpoint.base_sha !== cycle.base_development_sha || checkpoint.sha === cycle.candidate_sha
  ) {
    throw new Error("Sentinel manual-checkpoint cycle does not match its immutable candidate");
  }
  return {
    schema_version: 1,
    run_id: expected.runId,
    started_at: cycle.started_at,
    base_development_sha: cycle.base_development_sha,
    candidate_sha: cycle.candidate_sha,
    temporary_branch: cycle.temporary_branch,
    status: expected.status,
    stage: expected.stage,
    branch_disposition: cycle.branch_disposition as SentinelManualRequiredCycleReport["branch_disposition"],
    retry_checkpoint: checkpoint,
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
    checkpoint === null ||
    (cycle.branch_disposition !== "remote_retained_issue_retry_pending" &&
      cycle.branch_disposition !== "remote_retained_atomic_push_in_flight" &&
      cycle.branch_disposition !== "atomic_retry_push_requires_reconciliation") ||
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
      (typeof cycle.evidence_artifact_name !== "string" || cycle.evidence_artifact_name.length > 240)) ||
    (cycle.base_development_sha !== undefined && cycle.base_development_sha !== null &&
      (typeof cycle.base_development_sha !== "string" || !FULL_SHA.test(cycle.base_development_sha)))
  ) throw new Error("Sentinel cycle report is invalid");
  return {
    schema_version: 1,
    run_id: cycle.run_id,
    candidate_sha: cycle.candidate_sha,
    temporary_branch: cycle.temporary_branch,
    status: cycle.status,
    stage: cycle.stage,
    evidence_artifact_name: cycle.evidence_artifact_name,
    base_development_sha: cycle.base_development_sha ?? null,
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
      (localRef !== "HEAD" && !localRef.startsWith("refs/") && !FULL_SHA.test(localRef)) ||
      !remoteRef.startsWith("refs/") ||
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

export const selectRetryCheckpointPush = (
  updates: readonly GitPushUpdate[],
  checkpoint: GitHubIssueRetryCheckpointReport,
): GitPushUpdate => {
  const matches = updates.filter((update) => update.remoteRef === `refs/heads/${checkpoint.branch}`);
  if (
    matches.length !== 1 || matches[0]!.localRef !== checkpoint.sha ||
    matches[0]!.localSha !== checkpoint.sha
  ) {
    throw new Error("Sentinel retry-pending push has no exact checkpoint update");
  }
  return matches[0]!;
};

export const selectManualCheckpointPush = (
  updates: readonly GitPushUpdate[],
  checkpoint: GitHubIssueManualCheckpointReport,
): GitPushUpdate => {
  const matches = updates.filter((update) => update.remoteRef === `refs/heads/${checkpoint.branch}`);
  if (
    matches.length !== 1 || matches[0]!.localRef !== checkpoint.sha ||
    matches[0]!.localSha !== checkpoint.sha
  ) {
    throw new Error("Sentinel manual-checkpoint push has no exact candidate update");
  }
  return matches[0]!;
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
