import { GitHubActionsClient, type GitHubIssue } from "./github.ts";
import {
  getCurrentGitHubIssueJob,
  type GitHubIssueJobSource,
  isSentinelInertIssueComment,
  parseGitHubIssueJobLedger,
  renderGitHubIssueJobLedger,
} from "./issues.ts";
import {
  evaluateIssueCompletionAction,
  type GitHubIssuePullRequestRecord,
  type GitHubIssueSelectionReport,
  isContainedDevelopmentComparison,
  isPullRequestMergeRefusalStatus,
  isSentinelRecoveryCandidateBranch,
  ISSUE_COMPLETION_EVIDENCE_TEXT,
  issueEvidenceMarker,
  parseGitHubIssueManualRequiredReport,
  parseGitHubIssueManualRequiredRetainedCheckpointReport,
  parseGitHubIssuePullRequestRecord,
  parseGitHubIssueRetryPendingReport,
  parseGitHubIssueSelectionReportAny,
  parseSentinelCycleReport,
  parseSentinelManualRequiredCycleReport,
  parseSentinelManualRequiredRetainedCheckpointCycleReport,
  parseSentinelRetryPendingCycleReport,
  renderIssueDeliveryEvidence,
  sentinelRecoveryCandidateBranch,
  validateRetryPendingCheckpointPhaseBinding,
  verifyFrozenIssuePlanDigest,
} from "./issue-delivery.ts";
import { assertMatrixCycleReportV1, parseMatrixPlanV1, validateMatrixCycleReportV1 } from "./matrix.ts";
import {
  assertSentinelRecoveryTransition,
  isTerminalRecoveryPhase,
  parseSentinelRecoveryRecord,
  type SentinelRecoveryDisposition,
  type SentinelRecoveryPhase,
  type SentinelRecoveryRecordV1,
} from "./recovery.ts";

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const SAFE_REVISION = /^[A-Za-z0-9_-]{1,200}$/u;

type PullRequest = Readonly<{
  number: number;
  state: "open" | "closed";
  mergedAt: string | null;
  headRef: string;
  headSha: string;
  baseRef: string;
}>;

type Comment = Readonly<{
  id: number;
  body: string;
  updatedAt: string;
  authorLogin: string;
  authorType: string;
}>;
type CompletionEvidenceIdentity = Readonly<{ id: number; updatedAt: string }>;

type IssueState = Readonly<{
  state: "open" | "closed";
  stateReason: string | null;
}>;

type PullRequestMergeSource = "sentinel_merge_api" | "development_content" | "already_merged" | null;
type RetryPendingRemoteRefs = Readonly<{ developmentSha: string; checkpointSha: string }>;
type RetryPendingIssueReconciliationInput = Readonly<{
  workflowRunId: string;
  workflowFailed: boolean;
  selection: GitHubIssueSelectionReport;
  cycleValue: unknown;
  dispositionValue: unknown;
  pullRequestReportPresent: boolean;
  productionOutcomeReportPresent: boolean;
  developmentLedgerMarkdown: string;
  remoteRefs?: RetryPendingRemoteRefs;
}>;
type ManualRequiredRetainedCheckpointReconciliationInput = Readonly<{
  workflowRunId: string;
  selection: GitHubIssueSelectionReport;
  cycleValue: unknown;
  dispositionValue: unknown;
  pullRequestReportPresent: boolean;
  productionOutcomeReportPresent: boolean;
  developmentLedgerMarkdown: string;
  remoteRefs: RetryPendingRemoteRefs;
}>;
type NativeReviewExhaustedManualCheckpointReconciliationInput = Readonly<{
  workflowRunId: string;
  workflowRunAttempt: number;
  workflowFailed: boolean;
  selection: GitHubIssueSelectionReport;
  cycleValue: unknown;
  dispositionValue: unknown;
  pullRequestReportPresent: boolean;
  productionOutcomeReportPresent: boolean;
  developmentLedgerMarkdown: string;
  remoteRefs: RetryPendingRemoteRefs;
}>;
const MAX_COMMENT_TIMESTAMP_PROPAGATION_MS = 5_000;

/**
 * The remote facts needed to reconcile one recovery record. GitHub/API
 * adapters should populate this from exact commit and pull-request identities;
 * absence is represented by null rather than by an invented SHA.
 */
export type SentinelRecoveryDeliveryObservation = Readonly<{
  pull_request_number: number;
  head_branch: string;
  head_sha: string;
  base_branch: "development";
  head_contained_in_development: boolean;
  state: "open" | "closed";
  merged_at: string | null;
}>;

export type SentinelRecoveryRemoteObservation = Readonly<{
  candidate_branch: string | null;
  candidate_sha: string | null;
  development_sha: string | null;
  deliveries: readonly SentinelRecoveryDeliveryObservation[];
}>;

export type SentinelRecoveryReconciliationAction =
  | "terminal_noop"
  | "await_evidence"
  | "retry_checkpoint_push"
  | "checkpoint_confirmed"
  | "resume_validation"
  | "resume_review"
  | "delivery_confirmed"
  | "reject_no_candidate_diff"
  | "manual_required";

export type SentinelRecoveryReconciliationResult = Readonly<{
  before: SentinelRecoveryRecordV1;
  after: SentinelRecoveryRecordV1;
  changed: boolean;
  action: SentinelRecoveryReconciliationAction;
  candidate_branch: string | null;
  disposition: SentinelRecoveryDisposition;
  reason: string | null;
}>;

export type SentinelRecoveryReconciliationInput = Readonly<{
  record: unknown;
  remote?: SentinelRecoveryRemoteObservation;
  now?: string;
  expected_state_version?: number;
  expected_lease_token?: string;
}>;

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required for Sentinel issue reconciliation`);
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

const regularFileExists = async (path: string): Promise<boolean> => {
  try {
    const information = await Deno.lstat(path);
    if (!information.isFile || information.isSymlink) {
      throw new Error("Sentinel reconciliation report paths must be regular files");
    }
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return false;
    throw error;
  }
};

const record = (value: unknown): Record<string, unknown> | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;

const validRecoveryTimestamp = (value: unknown): value is string =>
  typeof value === "string" && /^\d{4}-\d{2}-\d{2}T/u.test(value) && Number.isFinite(Date.parse(value));

const RECOVERY_SOURCE_KINDS = new Set(
  [
    "github_issue",
    "review_backlog",
    "triage",
    "incident",
  ] as const,
);

/**
 * Reconstruct the deterministic candidate branch of the records a recovery
 * record links to through `predecessor`. The key is the canonical JSON
 * identity array produced by the ledger, so an attacker never controls a
 * branch name: only a well-formed known identity yields one.
 */
const recoveryBranchFromPredecessor = (predecessorKey: string): string | null => {
  try {
    const value = JSON.parse(predecessorKey) as unknown;
    if (
      !Array.isArray(value) || value.length !== 5 || typeof value[0] !== "string" ||
      typeof value[1] !== "string" ||
      !RECOVERY_SOURCE_KINDS.has(value[1] as SentinelRecoveryRecordV1["identity"]["source_kind"]) ||
      typeof value[2] !== "string" || typeof value[3] !== "string" ||
      !Number.isSafeInteger(value[4]) || (value[4] as number) <= 0
    ) return null;
    return sentinelRecoveryCandidateBranch({
      repository: value[0],
      source_kind: value[1] as SentinelRecoveryRecordV1["identity"]["source_kind"],
      source_id: value[2],
      source_revision: value[3],
      candidate_generation: value[4] as number,
    });
  } catch {
    return null;
  }
};

const parseRecoveryRemoteObservation = (
  value: SentinelRecoveryRemoteObservation,
): SentinelRecoveryRemoteObservation => {
  if (
    value === null || typeof value !== "object" ||
    (value.candidate_branch !== null &&
      (typeof value.candidate_branch !== "string" || !isSentinelRecoveryCandidateBranch(value.candidate_branch))) ||
    (value.candidate_sha !== null &&
      (typeof value.candidate_sha !== "string" || !FULL_SHA.test(value.candidate_sha))) ||
    (value.candidate_branch === null && value.candidate_sha !== null) ||
    (value.development_sha !== null &&
      (typeof value.development_sha !== "string" || !FULL_SHA.test(value.development_sha))) ||
    !Array.isArray(value.deliveries)
  ) {
    throw new Error("Sentinel recovery remote observation is invalid");
  }
  const deliveries = value.deliveries.map((delivery) => {
    if (
      delivery === null || typeof delivery !== "object" ||
      !Number.isSafeInteger(delivery.pull_request_number) || delivery.pull_request_number <= 0 ||
      typeof delivery.head_branch !== "string" || !SAFE_BRANCH.test(delivery.head_branch) ||
      typeof delivery.head_sha !== "string" || !FULL_SHA.test(delivery.head_sha) ||
      delivery.base_branch !== "development" || typeof delivery.head_contained_in_development !== "boolean" ||
      (delivery.state !== "open" && delivery.state !== "closed") ||
      (delivery.merged_at !== null &&
        (typeof delivery.merged_at !== "string" || !validRecoveryTimestamp(delivery.merged_at))) ||
      (delivery.state === "open" && delivery.merged_at !== null)
    ) {
      throw new Error("Sentinel recovery delivery observation is invalid");
    }
    return {
      pull_request_number: delivery.pull_request_number,
      head_branch: delivery.head_branch,
      head_sha: delivery.head_sha,
      base_branch: "development" as const,
      head_contained_in_development: delivery.head_contained_in_development,
      state: delivery.state,
      merged_at: delivery.merged_at,
    };
  });
  return {
    candidate_branch: value.candidate_branch,
    candidate_sha: value.candidate_sha,
    development_sha: value.development_sha,
    deliveries,
  };
};

const terminalDisposition = (phase: SentinelRecoveryPhase): SentinelRecoveryDisposition =>
  phase === "manual_required" || phase === "rejected" || phase === "delivered" ? phase : "active";

const transitionRecoveryRecord = (
  current: SentinelRecoveryRecordV1,
  phase: SentinelRecoveryPhase,
  now: string,
  patch: Readonly<Partial<SentinelRecoveryRecordV1>> = {},
): SentinelRecoveryRecordV1 => {
  if (phase === current.phase) return current;
  const next: SentinelRecoveryRecordV1 = {
    ...current,
    ...patch,
    phase,
    disposition: terminalDisposition(phase),
    state_version: current.state_version + 1,
    updated_at: now,
  };
  assertSentinelRecoveryTransition(current, next);
  return parseSentinelRecoveryRecord(next);
};

const recoveryResult = (
  before: SentinelRecoveryRecordV1,
  after: SentinelRecoveryRecordV1,
  action: SentinelRecoveryReconciliationAction,
  candidateBranch: string | null,
  reason: string | null = after.reason,
): SentinelRecoveryReconciliationResult => ({
  before,
  after,
  changed: after.state_version !== before.state_version,
  action,
  candidate_branch: candidateBranch,
  disposition: after.disposition,
  reason,
});

const recoveryManual = (
  current: SentinelRecoveryRecordV1,
  now: string,
  branch: string | null,
  reason: string,
): SentinelRecoveryReconciliationResult => {
  if (current.phase === "manual_required") return recoveryResult(current, current, "terminal_noop", branch);
  if (isTerminalRecoveryPhase(current.phase)) {
    throw new Error(`Sentinel terminal recovery record conflicts with reconciliation: ${reason}`);
  }
  const next = transitionRecoveryRecord(current, "manual_required", now, {
    failure_class: current.failure_class ?? "reconciliation_integrity",
    reason,
    next_action: "Owner review is required before another Sentinel attempt.",
  });
  return recoveryResult(current, next, "manual_required", branch, reason);
};

/**
 * Compare-and-swap guard for a durable recovery record. A caller must read the
 * record, retain its state version and lease, and only publish a transition if
 * both still match. This keeps an overlapping reconciler from overwriting a
 * newer disposition.
 */
export const compareAndSwapSentinelRecoveryRecord = (
  input: Readonly<{
    current: unknown;
    next: unknown;
    expected_state_version: number;
    expected_lease_token: string;
  }>,
): SentinelRecoveryRecordV1 => {
  const current = parseSentinelRecoveryRecord(input.current);
  const next = parseSentinelRecoveryRecord(input.next);
  if (!Number.isSafeInteger(input.expected_state_version) || input.expected_state_version <= 0) {
    throw new Error("Sentinel recovery compare-and-swap state version is invalid");
  }
  if (typeof input.expected_lease_token !== "string" || input.expected_lease_token.trim().length === 0) {
    throw new Error("Sentinel recovery compare-and-swap lease is invalid");
  }
  if (
    current.state_version !== input.expected_state_version || current.lease_token !== input.expected_lease_token
  ) {
    throw new Error("Sentinel recovery compare-and-swap lost its lease or observed a newer state");
  }
  assertSentinelRecoveryTransition(current, next);
  return next;
};

/**
 * Reconcile one immutable recovery identity from durable local state and exact
 * remote observations. The function is deliberately side-effect free: the
 * workflow/store owns persistence and must use the CAS helper above. Reusing
 * the returned record on a later invocation is idempotent and never allocates a
 * second branch or delivery.
 */
export const reconcileSentinelRecoveryRecord = (
  input: SentinelRecoveryReconciliationInput,
): SentinelRecoveryReconciliationResult => {
  const current = parseSentinelRecoveryRecord(input.record);
  if (
    input.expected_state_version !== undefined &&
    (current.state_version !== input.expected_state_version ||
      !Number.isSafeInteger(input.expected_state_version) || input.expected_state_version <= 0)
  ) {
    throw new Error("Sentinel recovery reconciliation observed a newer state version");
  }
  if (
    input.expected_lease_token !== undefined &&
    (typeof input.expected_lease_token !== "string" ||
      input.expected_lease_token.trim().length === 0 || current.lease_token !== input.expected_lease_token)
  ) {
    throw new Error("Sentinel recovery reconciliation lost its lease");
  }
  if (isTerminalRecoveryPhase(current.phase)) {
    if (input.remote !== undefined) {
      const remote = parseRecoveryRemoteObservation(input.remote);
      if (current.phase === "delivered" && remote.deliveries.length > 1) {
        throw new Error("Sentinel terminal recovery record has duplicate delivery observations");
      }
    }
    return recoveryResult(current, current, "terminal_noop", current.candidate_branch);
  }

  const now = input.now ?? new Date().toISOString();
  if (!validRecoveryTimestamp(now) || Date.parse(now) < Date.parse(current.updated_at)) {
    throw new Error("Sentinel recovery reconciliation timestamp moved backwards");
  }
  const remote = input.remote === undefined ? null : parseRecoveryRemoteObservation(input.remote);
  const expectedBranch = sentinelRecoveryCandidateBranch(current.identity);
  // A canonical parent record may point through `predecessor` at its exact
  // child record whose deterministic branch carries the recovery candidate.
  const linkedBranch = current.predecessor === null ? null : recoveryBranchFromPredecessor(current.predecessor);
  const deterministicBranch = (branch: string): boolean =>
    branch === expectedBranch || (linkedBranch !== null && branch === linkedBranch);
  let candidateBranch = current.candidate_branch;
  if (candidateBranch !== null && !deterministicBranch(candidateBranch)) {
    return recoveryManual(current, now, candidateBranch, "Recorded recovery candidate branch is not deterministic.");
  }
  if (remote !== null && remote.candidate_branch !== null) {
    if (candidateBranch !== null && remote.candidate_branch !== candidateBranch) {
      return recoveryManual(
        current,
        now,
        candidateBranch,
        "Remote candidate branch changed from the recorded identity.",
      );
    }
    if (candidateBranch === null && !deterministicBranch(remote.candidate_branch)) {
      return recoveryManual(
        current,
        now,
        remote.candidate_branch,
        "Remote candidate branch is not the deterministic recovery identity.",
      );
    }
    candidateBranch ??= remote.candidate_branch;
  }
  candidateBranch ??= expectedBranch;

  const observedCandidateSha = remote?.candidate_sha ?? null;
  if (
    current.candidate_sha !== null && observedCandidateSha !== null && current.candidate_sha !== observedCandidateSha
  ) {
    return recoveryManual(current, now, candidateBranch, "Remote candidate SHA changed from the recorded identity.");
  }
  const candidateSha = current.candidate_sha ?? observedCandidateSha;
  const deliveries = remote?.deliveries ?? [];
  if (deliveries.length > 1) {
    return recoveryManual(
      current,
      now,
      candidateBranch,
      "More than one delivery pull request matches the recovery identity.",
    );
  }
  const delivery = deliveries[0] ?? null;
  if (
    delivery !== null &&
    (candidateSha === null || delivery.head_branch !== candidateBranch || delivery.head_sha !== candidateSha)
  ) {
    return recoveryManual(
      current,
      now,
      candidateBranch,
      "Delivery pull request identity does not match the recovery candidate.",
    );
  }
  if (delivery !== null && delivery.merged_at !== null && !delivery.head_contained_in_development) {
    return recoveryManual(
      current,
      now,
      candidateBranch,
      "Merged recovery delivery is not proven in development ancestry.",
    );
  }

  // A candidate tip that is already the development tip may have been pushed
  // directly during an ambiguous delivery attempt. Without a matching PR,
  // never manufacture another delivery record or silently mark it delivered.
  if (remote?.development_sha !== null && remote?.development_sha === candidateSha && delivery === null) {
    return recoveryManual(
      current,
      now,
      candidateBranch,
      "The candidate tip is already development without an exact delivery record.",
    );
  }

  const hasRemoteCandidate = remote !== null && remote.candidate_branch !== null && observedCandidateSha !== null;
  const candidatePatch: Partial<SentinelRecoveryRecordV1> = {
    candidate_branch: candidateBranch,
    ...(candidateSha === null ? {} : { candidate_sha: candidateSha }),
  };

  if (delivery !== null && delivery.state === "closed" && delivery.merged_at !== null) {
    if (current.phase === "review_pending") {
      const next = transitionRecoveryRecord(current, "delivered", now, {
        ...candidatePatch,
        reason: "Delivery pull request is durably merged with the recorded candidate identity.",
        next_action: null,
      });
      return recoveryResult(current, next, "delivery_confirmed", candidateBranch);
    }
    if (current.phase === "checkpoint_durable") {
      const next = transitionRecoveryRecord(current, "review_pending", now, candidatePatch);
      return recoveryResult(current, next, "resume_review", candidateBranch);
    }
  }

  if (hasRemoteCandidate) {
    if (current.phase === "recovery_pending" || current.phase === "checkpoint_publishing") {
      const next = transitionRecoveryRecord(current, "checkpoint_durable", now, {
        ...candidatePatch,
        reason: "The remote candidate ref matches the recorded candidate SHA.",
        next_action: "Validate the durable checkpoint before review or delivery.",
      });
      return recoveryResult(current, next, "checkpoint_confirmed", candidateBranch);
    }
    if (
      current.phase === "claimed" || current.phase === "implementation_running" ||
      current.phase === "workspace_dirty" ||
      current.phase === "retry_wait"
    ) {
      const next = transitionRecoveryRecord(current, "recovery_pending", now, {
        ...candidatePatch,
        reason: "A remote candidate ref was found for the incomplete recovery record.",
        next_action: "Validate the durable checkpoint before review or delivery.",
      });
      return recoveryResult(current, next, "checkpoint_confirmed", candidateBranch);
    }
    if (current.phase === "validation_failed") {
      const next = transitionRecoveryRecord(current, "retry_wait", now, {
        ...candidatePatch,
        reason: "The failed candidate checkpoint is still remotely durable and may be retried.",
        next_action: "Retry validation according to the bounded failure policy.",
      });
      return recoveryResult(current, next, "resume_validation", candidateBranch);
    }
    if (current.phase === "checkpoint_durable") {
      return recoveryResult(
        current,
        current,
        delivery === null ? "resume_validation" : "resume_review",
        candidateBranch,
      );
    }
    if (current.phase === "review_pending") {
      return recoveryResult(current, current, delivery === null ? "resume_review" : "resume_review", candidateBranch);
    }
  }

  if (delivery !== null) {
    return recoveryManual(current, now, candidateBranch, "A delivery pull request is closed without a verified merge.");
  }

  if (candidateSha !== null) {
    if (remote === null) {
      return recoveryResult(
        current,
        current,
        "await_evidence",
        candidateBranch,
        "Remote candidate evidence is absent.",
      );
    }
    if (current.phase === "review_pending") {
      const next = transitionRecoveryRecord(current, "manual_required", now, {
        ...candidatePatch,
        failure_class: current.failure_class ?? "git_publication_ambiguity",
        reason: "The recorded candidate ref is no longer visible remotely after review became pending.",
        next_action: "Owner must prove or restore the candidate ref before delivery.",
      });
      return recoveryResult(current, next, "manual_required", candidateBranch);
    }
    if (current.phase === "validation_failed") {
      const next = transitionRecoveryRecord(current, "retry_wait", now, {
        ...candidatePatch,
        reason: "Candidate publication remains ambiguous after validation failed.",
        next_action: "Retry the checkpoint push according to the bounded failure policy.",
      });
      return recoveryResult(current, next, "retry_checkpoint_push", candidateBranch);
    }
    if (current.phase !== "recovery_pending") {
      const next = transitionRecoveryRecord(current, "recovery_pending", now, {
        ...candidatePatch,
        failure_class: current.failure_class ?? "git_publication_ambiguity",
        reason: "Candidate publication is ambiguous; retry the same branch identity.",
        next_action: "Retry the checkpoint push with the recorded branch and SHA.",
      });
      return recoveryResult(current, next, "retry_checkpoint_push", candidateBranch);
    }
    return recoveryResult(current, current, "retry_checkpoint_push", candidateBranch);
  }

  if (current.changed_files.length === 0 && remote !== null && remote.candidate_branch === null) {
    if (
      current.phase === "claimed" || current.phase === "implementation_running" || current.phase === "review_pending" ||
      current.phase === "recovery_pending" || current.phase === "validation_failed"
    ) {
      const next = transitionRecoveryRecord(current, "rejected", now, {
        candidate_branch: candidateBranch,
        reason: "rejected/no_candidate_diff",
        next_action: null,
      });
      return recoveryResult(current, next, "reject_no_candidate_diff", candidateBranch);
    }
    const next = transitionRecoveryRecord(current, "recovery_pending", now, {
      candidate_branch: candidateBranch,
      reason: "No candidate diff is durable yet; reconcile the incomplete record again.",
      next_action: "Confirm the absence of a candidate diff before rejection.",
    });
    return recoveryResult(current, next, "await_evidence", candidateBranch);
  }

  if (current.phase === "recovery_pending") {
    return recoveryResult(current, current, "await_evidence", candidateBranch);
  }
  if (current.phase === "review_pending") {
    return recoveryManual(
      current,
      now,
      candidateBranch,
      "Review is pending but no exact candidate delivery was observed.",
    );
  }
  if (current.phase === "validation_failed") {
    const next = transitionRecoveryRecord(current, "retry_wait", now, {
      candidate_branch: candidateBranch,
      reason: "Validation failed before a durable candidate was observed.",
      next_action: "Retry the bounded recovery attempt or require manual review.",
    });
    return recoveryResult(current, next, "await_evidence", candidateBranch);
  }
  const next = transitionRecoveryRecord(current, "recovery_pending", now, {
    candidate_branch: candidateBranch,
    reason: "The record is incomplete and requires another bounded reconciliation pass.",
    next_action: "Reconcile the recorded branch and candidate SHA.",
  });
  return recoveryResult(current, next, "await_evidence", candidateBranch);
};

const githubRequestRaw = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
  fetcher: typeof fetch = fetch,
): Promise<Readonly<{ status: number; payload: unknown }>> => {
  const response = await fetcher(`https://api.github.com/repos/${repository}${path}`, {
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
  let payload: unknown = null;
  if (text.length > 0) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = null;
    }
  }
  return { status: response.status, payload };
};

const githubRequest = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit = {},
  fetcher: typeof fetch = fetch,
): Promise<unknown> => {
  const { status, payload } = await githubRequestRaw(token, repository, path, init, fetcher);
  if (status < 200 || status >= 300) {
    const formatted = typeof payload === "string" ? payload : JSON.stringify(payload);
    throw new Error(
      `GitHub API ${init.method ?? "GET"} ${path} failed with HTTP ${status}: ${
        formatted === "null" ? "" : formatted.slice(0, 500)
      }`,
    );
  }
  return payload;
};

export const readGitHubCommitRefSha = async (
  token: string,
  repository: string,
  branch: string,
  fetcher: typeof fetch = fetch,
): Promise<string> => {
  if (branch !== "development" && !isSentinelRecoveryCandidateBranch(branch)) {
    throw new Error("Sentinel reconciliation requires a trusted Git ref name");
  }
  const ref = `refs/heads/${branch}`;
  const value = record(await githubRequest(token, repository, `/git/ref/heads/${branch}`, {}, fetcher));
  const object = record(value?.object);
  if (
    value?.ref !== ref || object?.type !== "commit" || typeof object.sha !== "string" || !FULL_SHA.test(object.sha)
  ) {
    throw new Error(`GitHub returned an invalid commit identity for ${ref}`);
  }
  return object.sha;
};

const parsePullRequest = (value: unknown): PullRequest => {
  const pull = record(value);
  const head = record(pull?.head);
  const base = record(pull?.base);
  if (
    !pull || !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
    (pull.state !== "open" && pull.state !== "closed") ||
    (pull.merged_at !== null && typeof pull.merged_at !== "string") || !head || !base ||
    typeof head.ref !== "string" || typeof head.sha !== "string" || !FULL_SHA.test(head.sha) ||
    typeof base.ref !== "string"
  ) {
    throw new Error("GitHub returned an invalid pull request during issue reconciliation");
  }
  return {
    number: pull.number as number,
    state: pull.state,
    mergedAt: pull.merged_at as string | null,
    headRef: head.ref,
    headSha: head.sha,
    baseRef: base.ref,
  };
};

const fetchIssuePullRequest = async (
  token: string,
  repository: string,
  expected: GitHubIssuePullRequestRecord,
  fetcher: typeof fetch,
): Promise<PullRequest> => {
  const pull = parsePullRequest(
    await githubRequest(token, repository, `/pulls/${expected.pull_request_number}`, {}, fetcher),
  );
  if (
    pull.number !== expected.pull_request_number || pull.headRef !== expected.head_branch ||
    pull.headSha !== expected.head_sha || pull.baseRef !== expected.base_branch
  ) {
    throw new Error("Sentinel issue pull request changed identity before reconciliation");
  }
  return pull;
};

export const mergeDeliveryPullRequest = async (
  token: string,
  repository: string,
  expected: GitHubIssuePullRequestRecord,
  fetcher: typeof fetch = fetch,
): Promise<Readonly<{ source: "sentinel_merge_api" | "development_content" | "already_merged" }>> => {
  const pull = await fetchIssuePullRequest(token, repository, expected, fetcher);
  if (pull.state === "closed" && pull.mergedAt !== null) return { source: "already_merged" };
  const response = await githubRequestRaw(token, repository, `/pulls/${expected.pull_request_number}/merge`, {
    method: "PUT",
    body: JSON.stringify({
      merge_method: "merge",
      commit_title: `merge: Provider Sentinel deliverable for #${expected.issue_number}`,
      sha: expected.head_sha,
    }),
  }, fetcher);
  const mergedPayload = record(response.payload);
  if (response.status === 200 && mergedPayload?.merged === true) return { source: "sentinel_merge_api" };
  if (isPullRequestMergeRefusalStatus(response.status)) {
    // A refusal can race with a force-push or other head update after the
    // initial identity check. Revalidate before treating the recorded head as
    // already delivered through development.
    await fetchIssuePullRequest(token, repository, expected, fetcher);
    // The candidate commits were pushed directly to development, so the head
    // may already be contained in the base. GitHub can refuse the API merge in
    // that state (or under branch protection); the comparison decides whether
    // the delivery is genuinely already integrated.
    const comparison = record(
      await githubRequest(token, repository, `/compare/development...${expected.head_sha}`, {}, fetcher),
    );
    const compareStatus = typeof comparison?.status === "string" ? comparison.status : null;
    if (compareStatus !== null && isContainedDevelopmentComparison(compareStatus)) {
      return { source: "development_content" };
    }
  }
  throw new Error(
    `Sentinel merge of pull request #${expected.pull_request_number} failed with HTTP ${response.status}`,
  );
};

const listComments = async (
  token: string,
  repository: string,
  issueNumber: number,
): Promise<Comment[]> => {
  const comments: Comment[] = [];
  for (let page = 1; page <= 10; page++) {
    const value = await githubRequest(
      token,
      repository,
      `/issues/${issueNumber}/comments?per_page=100&page=${page}`,
    );
    if (!Array.isArray(value)) throw new Error("GitHub issue-comment listing is invalid");
    for (const item of value) {
      const comment = record(item);
      const author = record(comment?.user);
      if (
        !comment || !Number.isSafeInteger(comment.id) || (comment.id as number) <= 0 ||
        typeof comment.body !== "string" || typeof comment.updated_at !== "string" ||
        !Number.isFinite(Date.parse(comment.updated_at)) || !author ||
        typeof author.login !== "string" || typeof author.type !== "string"
      ) {
        throw new Error("GitHub returned an invalid issue comment");
      }
      comments.push({
        id: comment.id as number,
        body: comment.body,
        updatedAt: comment.updated_at,
        authorLogin: author.login,
        authorType: author.type,
      });
    }
    if (value.length < 100) return comments;
  }
  throw new Error("Sentinel issue-comment lookup exceeded its pagination limit");
};

const upsertComment = async (
  token: string,
  repository: string,
  issueNumber: number,
  marker: string,
  body: string,
): Promise<void> => {
  const matching = (await listComments(token, repository, issueNumber)).filter((comment) =>
    comment.authorLogin === "github-actions[bot]" && comment.authorType === "Bot" && comment.body.includes(marker)
  );
  if (matching.length > 1) throw new Error("Sentinel evidence has more than one matching GitHub comment");
  if (matching.length === 1) {
    await githubRequest(token, repository, `/issues/comments/${matching[0]!.id}`, {
      method: "PATCH",
      body: JSON.stringify({ body }),
    });
  } else {
    await githubRequest(token, repository, `/issues/${issueNumber}/comments`, {
      method: "POST",
      body: JSON.stringify({ body }),
    });
  }
};

const getIssueState = async (
  token: string,
  repository: string,
  issueNumber: number,
): Promise<IssueState> => {
  const value = record(await githubRequest(token, repository, `/issues/${issueNumber}`));
  if (
    !value || (value.state !== "open" && value.state !== "closed") ||
    (value.state_reason !== null && typeof value.state_reason !== "string")
  ) {
    throw new Error("GitHub returned an invalid issue state");
  }
  return {
    state: value.state,
    stateReason: value.state_reason as string | null,
  };
};

const completionEvidence = async (
  token: string,
  repository: string,
  issueNumber: number,
  marker: string,
): Promise<Comment | null> => {
  const matching = (await listComments(token, repository, issueNumber)).filter((comment) =>
    comment.authorLogin === "github-actions[bot]" && comment.authorType === "Bot" &&
    comment.body.includes(marker) && comment.body.includes(ISSUE_COMPLETION_EVIDENCE_TEXT)
  );
  if (matching.length > 1) throw new Error("Sentinel completion evidence is duplicated");
  return matching[0] ?? null;
};

const githubIssueSnapshotsMatch = (expected: GitHubIssue, actual: GitHubIssue): boolean =>
  expected.id === actual.id && expected.nodeId === actual.nodeId && expected.number === actual.number &&
  expected.state === actual.state && expected.title === actual.title && expected.body === actual.body &&
  expected.htmlUrl === actual.htmlUrl && expected.authorLogin === actual.authorLogin &&
  expected.authorAssociation === actual.authorAssociation && expected.locked === actual.locked &&
  expected.comments === actual.comments && expected.createdAt === actual.createdAt &&
  expected.updatedAt === actual.updatedAt && expected.isPullRequest === actual.isPullRequest &&
  JSON.stringify(expected.labels) === JSON.stringify(actual.labels) &&
  JSON.stringify(expected.assignees) === JSON.stringify(actual.assignees);

export const completionEvidenceSnapshotMatches = async (
  source: GitHubIssueJobSource,
  repository: string,
  selection: GitHubIssueSelectionReport,
  issue: GitHubIssue,
  evidence: CompletionEvidenceIdentity,
): Promise<boolean> => {
  const evidenceUpdatedMs = Date.parse(evidence.updatedAt);
  const issueUpdatedMs = Date.parse(issue.updatedAt);
  if (selection.comments === Number.MAX_SAFE_INTEGER) return false;
  const commentsWithCompletionEvidence = selection.comments + 1;
  if (
    issue.state !== "open" || issue.number !== selection.issue_number ||
    issue.comments !== commentsWithCompletionEvidence ||
    !Number.isFinite(evidenceUpdatedMs) || !Number.isFinite(issueUpdatedMs) ||
    issueUpdatedMs < evidenceUpdatedMs || issueUpdatedMs - evidenceUpdatedMs > MAX_COMMENT_TIMESTAMP_PROPAGATION_MS
  ) return false;
  const comments = await source.listIssueComments(issue.number);
  if (comments.length !== issue.comments) return false;
  const completionComments = comments.filter((comment) => comment.id === evidence.id);
  const originalComments = comments.filter((comment) => comment.id !== evidence.id);
  if (
    completionComments.length !== 1 || completionComments[0]!.updatedAt !== evidence.updatedAt ||
    originalComments.length !== selection.comments || !originalComments.every(isSentinelInertIssueComment)
  ) return false;
  const normalizedIssue: GitHubIssue = {
    ...issue,
    comments: selection.comments,
    updatedAt: selection.updated_at,
  };
  const normalizedSource: GitHubIssueJobSource = {
    listOpenIssues: () => source.listOpenIssues(),
    getIssue: (issueNumber) =>
      issueNumber === selection.issue_number ? Promise.resolve(normalizedIssue) : source.getIssue(issueNumber),
    listIssueComments: (issueNumber) =>
      issueNumber === selection.issue_number
        ? Promise.resolve(originalComments)
        : source.listIssueComments(issueNumber),
    getIssueRelations: (issueNumber) => source.getIssueRelations(issueNumber),
    getRepositoryPermission: (username) => source.getRepositoryPermission(username),
  };
  const current = await getCurrentGitHubIssueJob(
    normalizedSource,
    repository,
    selection.issue_number,
  );
  return current !== null && current.issueId === selection.issue_id &&
    current.fingerprint === selection.fingerprint && current.bodySha256 === selection.body_sha256 &&
    current.updatedAt === selection.updated_at;
};

export const closeIssueAfterCompletionEvidenceRevalidation = async (
  source: GitHubIssueJobSource,
  repository: string,
  selection: GitHubIssueSelectionReport,
  evidence: CompletionEvidenceIdentity,
  close: () => Promise<void>,
): Promise<void> => {
  const issue = await source.getIssue(selection.issue_number);
  if (
    !await completionEvidenceSnapshotMatches(
      source,
      repository,
      selection,
      issue,
      evidence,
    )
  ) {
    throw new Error("Sentinel completion evidence no longer matches the open issue snapshot");
  }
  const finalIssue = await source.getIssue(selection.issue_number);
  if (!githubIssueSnapshotsMatch(issue, finalIssue)) {
    throw new Error("Sentinel completion evidence no longer matches the open issue snapshot");
  }
  await close();
};

const parseDisposition = (value: unknown): "resolved" | "manual_required" | null => {
  if (value === null) return null;
  const disposition = record(value);
  return disposition?.disposition === "resolved" || disposition?.disposition === "manual_required"
    ? disposition.disposition
    : null;
};

const parseOutcome = (value: unknown):
  | Readonly<{
    outcome: "kept" | "rolled_back";
    candidateSha: string;
    candidateRevision: string | null;
  }>
  | null => {
  if (value === null) return null;
  const outcome = record(value);
  if (
    !outcome || (outcome.outcome !== "kept" && outcome.outcome !== "rolled_back") ||
    typeof outcome.candidate_sha !== "string" || !FULL_SHA.test(outcome.candidate_sha) ||
    (outcome.candidate_revision !== null &&
      (typeof outcome.candidate_revision !== "string" || !SAFE_REVISION.test(outcome.candidate_revision)))
  ) {
    return null;
  }
  return {
    outcome: outcome.outcome,
    candidateSha: outcome.candidate_sha,
    candidateRevision: outcome.candidate_revision as string | null,
  };
};

/**
 * Prove that a production outcome pointing at a real ancestry-preserving merge
 * SHA matches the matrix cycle evidence and the exact GitHub pull request and
 * merge commit. Every identity check is exact; missing or invalid matrix
 * evidence fails closed before any network access, and there is no fallback to
 * a latest or ancestry-only match.
 */
export const validateMatrixIssueDeliveryMerge = async (
  input: Readonly<{
    token: string;
    repository: string;
    workflowRunId: string;
    workflowRunAttempt: number;
    pullRequest: GitHubIssuePullRequestRecord;
    outcome: "kept" | "rolled_back";
    deployedSha: string;
    matrixPlanValue: unknown;
    matrixCycleValue: unknown;
    fetcher?: typeof fetch;
  }>,
): Promise<void> => {
  if (!FULL_SHA.test(input.deployedSha)) {
    throw new Error("Matrix issue delivery merge SHA is invalid");
  }
  const planRaw = JSON.stringify(input.matrixPlanValue);
  if (planRaw === undefined) throw new Error("Matrix plan JSON is invalid");
  const plan = await parseMatrixPlanV1(planRaw);
  const cycleValue = input.matrixCycleValue;
  assertMatrixCycleReportV1(cycleValue, plan);
  const cycle = await validateMatrixCycleReportV1(cycleValue, plan);
  if (cycle.run_id !== input.workflowRunId || cycle.run_attempt !== input.workflowRunAttempt) {
    throw new Error("Matrix cycle report does not match the issuing workflow run");
  }
  const candidate = cycle.integrated_candidate;
  if (
    candidate === null || candidate.head_sha !== input.pullRequest.head_sha ||
    candidate.branch !== input.pullRequest.head_branch
  ) {
    throw new Error("Matrix cycle integrated candidate does not match the issue pull request");
  }
  if (
    cycle.delivery.pr_number !== input.pullRequest.pull_request_number ||
    cycle.delivery.merge_sha !== input.deployedSha
  ) {
    throw new Error("Matrix cycle delivery does not match the deployed merge");
  }
  if (cycle.delivery.status !== (input.outcome === "kept" ? "published" : "rolled_back")) {
    throw new Error("Matrix cycle delivery status does not match the production outcome");
  }
  const fetcher = input.fetcher ?? fetch;
  const rawPull = record(
    await githubRequest(
      input.token,
      input.repository,
      `/pulls/${input.pullRequest.pull_request_number}`,
      {},
      fetcher,
    ),
  );
  const pull = parsePullRequest(rawPull);
  if (
    pull.number !== input.pullRequest.pull_request_number ||
    pull.headRef !== input.pullRequest.head_branch || pull.headSha !== input.pullRequest.head_sha ||
    pull.baseRef !== input.pullRequest.base_branch || pull.state !== "closed" || pull.mergedAt === null ||
    typeof rawPull?.body !== "string" || !rawPull.body.includes(input.pullRequest.marker) ||
    rawPull?.merge_commit_sha !== input.deployedSha
  ) {
    throw new Error("Matrix issue pull request is not the exact delivered merge");
  }
  const commit = record(
    await githubRequest(input.token, input.repository, `/commits/${input.deployedSha}`, {}, fetcher),
  );
  const parents = commit?.parents;
  if (
    commit?.sha !== input.deployedSha || !Array.isArray(parents) || parents.length !== 2 ||
    record(parents[0])?.sha !== candidate.base_sha || record(parents[1])?.sha !== input.pullRequest.head_sha
  ) {
    throw new Error("Matrix deployed merge commit is not the exact ancestry-preserving merge");
  }
};

const productionWorkflowEvidence = async (
  reportsDir: string,
): Promise<
  Readonly<{
    deploymentRunId: number | null;
    promotionRunId: number | null;
    monitoringDecision: "keep" | "rollback" | null;
  }>
> => {
  const workflow = record(await optionalJson(`${reportsDir}/production-deployment-workflow.json`));
  const decision = record(await optionalJson(`${reportsDir}/production-decision.json`));
  return {
    deploymentRunId: Number.isSafeInteger(workflow?.deployment_workflow_run_id) &&
        (workflow!.deployment_workflow_run_id as number) > 0
      ? workflow!.deployment_workflow_run_id as number
      : null,
    promotionRunId: Number.isSafeInteger(workflow?.promotion_workflow_run_id) &&
        (workflow!.promotion_workflow_run_id as number) > 0
      ? workflow!.promotion_workflow_run_id as number
      : null,
    monitoringDecision: decision?.decision === "keep" || decision?.decision === "rollback" ? decision.decision : null,
  };
};

const closeIssue = async (token: string, repository: string, issueNumber: number): Promise<void> => {
  const value = record(
    await githubRequest(token, repository, `/issues/${issueNumber}`, {
      method: "PATCH",
      body: JSON.stringify({ state: "closed", state_reason: "completed" }),
    }),
  );
  if (value?.state !== "closed" || value.state_reason !== "completed") {
    throw new Error("GitHub issue did not close as completed");
  }
};

const bytesToBase64 = (bytes: Uint8Array): string => {
  let value = "";
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    value += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(value);
};

const developmentIssueLedger = async (
  token: string,
  repository: string,
  fetcher: typeof fetch = fetch,
): Promise<Readonly<{ blobSha: string; markdown: string }>> => {
  const value = record(
    await githubRequest(
      token,
      repository,
      "/contents/docs/sentinel-issue-jobs.md?ref=development",
      {},
      fetcher,
    ),
  );
  if (
    !value || typeof value.sha !== "string" || !FULL_SHA.test(value.sha) ||
    typeof value.content !== "string" || value.encoding !== "base64"
  ) {
    throw new Error("GitHub returned an invalid Sentinel issue ledger blob");
  }
  const normalized = value.content.replaceAll("\n", "");
  const markdown = new TextDecoder("utf-8", { fatal: true }).decode(
    Uint8Array.from(atob(normalized), (character) => character.charCodeAt(0)),
  );
  parseGitHubIssueJobLedger(markdown);
  return { blobSha: value.sha, markdown };
};

const removeRolledBackLedgerEntry = async (
  token: string,
  repository: string,
  issueNumber: number,
  fingerprint: string,
): Promise<void> => {
  const { blobSha, markdown } = await developmentIssueLedger(token, repository);
  const entries = parseGitHubIssueJobLedger(markdown);
  const retained = entries.filter((entry) => !(entry.number === issueNumber && entry.fingerprint === fingerprint));
  if (retained.length === entries.length) return;
  const content = bytesToBase64(new TextEncoder().encode(renderGitHubIssueJobLedger(retained)));
  await githubRequest(token, repository, "/contents/docs/sentinel-issue-jobs.md", {
    method: "PUT",
    body: JSON.stringify({
      message: `chore(sentinel): retry rolled-back issue #${issueNumber}`,
      content,
      sha: blobSha,
      branch: "development",
    }),
  });
};

const writeReconciliationReport = async (
  reportsDir: string,
  input: Readonly<{
    issueNumber: number;
    fingerprint: string;
    pullRequestNumber: number;
    pullRequestMerged: boolean;
    mergeSource: PullRequestMergeSource;
    action: string;
    issueSnapshotMatches: boolean;
    durableCompletionEvidenceReused: boolean;
  }>,
): Promise<void> => {
  await Deno.writeTextFile(
    `${reportsDir}/github-issue-reconciliation.json`,
    `${
      JSON.stringify(
        {
          schema_version: 1,
          issue_number: input.issueNumber,
          fingerprint: input.fingerprint,
          pull_request_number: input.pullRequestNumber,
          pull_request_merged: input.pullRequestMerged,
          merge_source: input.mergeSource,
          action: input.action,
          issue_snapshot_matches: input.issueSnapshotMatches,
          durable_completion_evidence_reused: input.durableCompletionEvidenceReused,
        },
        null,
        2,
      )
    }\n`,
    { mode: 0o600 },
  );
};

const parseRetryPendingIssueReconciliationReports = (
  input: RetryPendingIssueReconciliationInput,
): Readonly<{
  disposition: ReturnType<typeof parseGitHubIssueRetryPendingReport>;
  cycle: ReturnType<typeof parseSentinelRetryPendingCycleReport>;
}> => {
  const disposition = parseGitHubIssueRetryPendingReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = input.workflowFailed
    ? parseSentinelRetryPendingCycleReport(input.cycleValue, {
      runId: input.workflowRunId,
      status: "failed",
      stage: "failed",
      branchDispositions: ["atomic_retry_push_requires_reconciliation"],
    })
    : parseSentinelRetryPendingCycleReport(input.cycleValue, {
      runId: input.workflowRunId,
      status: "no_change",
      stage: "complete",
      branchDispositions: [
        "development_docs_only_issue_retry_pending",
        "remote_retained_issue_retry_pending",
      ],
    });
  return { disposition, cycle };
};

export const validateRetryPendingIssueReconciliation = (
  input: RetryPendingIssueReconciliationInput,
): void => {
  if (input.pullRequestReportPresent || input.productionOutcomeReportPresent) {
    throw new Error("A retry-pending issue reconciliation cannot contain delivery or production records");
  }
  const { disposition, cycle } = parseRetryPendingIssueReconciliationReports(input);
  if (
    cycle.candidate_sha === cycle.base_development_sha ||
    !cycle.temporary_branch.startsWith("sentinel/candidate-")
  ) {
    throw new Error("Sentinel retry-pending reconciliation has an invalid docs-only cycle identity");
  }
  const matches = parseGitHubIssueJobLedger(input.developmentLedgerMarkdown).filter((entry) =>
    entry.issueId === input.selection.issue_id && entry.number === input.selection.issue_number &&
    entry.fingerprint === input.selection.fingerprint
  );
  const expectedCheckpoint = disposition.retry_checkpoint === null ? null : {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  if (
    matches.length !== 1 || matches[0]!.bodySha256 !== input.selection.body_sha256 ||
    matches[0]!.comments !== input.selection.comments ||
    matches[0]!.sourceUpdatedAt !== input.selection.updated_at || matches[0]!.disposition !== "retry_pending" ||
    matches[0]!.baseSha !== (expectedCheckpoint?.baseSha ?? cycle.base_development_sha) ||
    JSON.stringify(matches[0]!.checkpoint) !== JSON.stringify(expectedCheckpoint) ||
    JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint) ||
    Date.parse(matches[0]!.recordedAt) < Date.parse(cycle.started_at)
  ) {
    throw new Error("Sentinel retry-pending reconciliation has no exact durable ledger row");
  }
  validateRetryPendingCheckpointPhaseBinding(disposition, cycle);
  if (!input.workflowFailed) return;
  if (disposition.retry_checkpoint === null) {
    throw new Error("A failed atomic retry push reconciliation requires a durable retry checkpoint");
  }
  const remoteRefs = input.remoteRefs;
  if (
    remoteRefs === undefined || !FULL_SHA.test(remoteRefs.developmentSha) || !FULL_SHA.test(remoteRefs.checkpointSha)
  ) {
    throw new Error("A failed atomic retry push reconciliation requires exact durable remote refs");
  }
  if (remoteRefs.developmentSha !== cycle.candidate_sha) {
    throw new Error("Sentinel failed atomic retry push development ref does not match the candidate commit");
  }
  if (remoteRefs.checkpointSha !== disposition.retry_checkpoint.sha) {
    throw new Error("Sentinel failed atomic retry push checkpoint ref does not match the durable checkpoint");
  }
};

const parseManualRequiredRetainedCheckpointReconciliationReports = (
  input: Pick<
    ManualRequiredRetainedCheckpointReconciliationInput,
    "workflowRunId" | "selection" | "cycleValue" | "dispositionValue"
  >,
): Readonly<{
  disposition: ReturnType<typeof parseGitHubIssueManualRequiredRetainedCheckpointReport>;
  cycle: ReturnType<typeof parseSentinelManualRequiredRetainedCheckpointCycleReport>;
}> => {
  const disposition = parseGitHubIssueManualRequiredRetainedCheckpointReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = parseSentinelManualRequiredRetainedCheckpointCycleReport(input.cycleValue, {
    runId: input.workflowRunId,
    status: "no_change",
    stage: "complete",
    branchDisposition: "development_docs_only_issue_manual_required",
  });
  return { disposition, cycle };
};

export const validateManualRequiredRetainedCheckpointReconciliation = (
  input: ManualRequiredRetainedCheckpointReconciliationInput,
): void => {
  if (input.pullRequestReportPresent || input.productionOutcomeReportPresent) {
    throw new Error("A retained-checkpoint manual reconciliation cannot contain delivery or production records");
  }
  const { disposition, cycle } = parseManualRequiredRetainedCheckpointReconciliationReports(input);
  if (JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint)) {
    throw new Error("Sentinel retained-checkpoint manual reconciliation has no exact checkpoint receipt");
  }
  const matches = parseGitHubIssueJobLedger(input.developmentLedgerMarkdown).filter((entry) =>
    entry.issueId === input.selection.issue_id && entry.number === input.selection.issue_number &&
    entry.fingerprint === input.selection.fingerprint
  );
  const expectedCheckpoint = {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  if (
    matches.length !== 1 || matches[0]!.bodySha256 !== input.selection.body_sha256 ||
    matches[0]!.comments !== input.selection.comments ||
    matches[0]!.sourceUpdatedAt !== input.selection.updated_at || matches[0]!.disposition !== "manual_required" ||
    matches[0]!.baseSha !== disposition.retry_checkpoint.base_sha ||
    JSON.stringify(matches[0]!.checkpoint) !== JSON.stringify(expectedCheckpoint) ||
    Date.parse(matches[0]!.recordedAt) < Date.parse(cycle.started_at)
  ) {
    throw new Error("Sentinel retained-checkpoint manual reconciliation has no exact durable ledger row");
  }
  if (
    !FULL_SHA.test(input.remoteRefs.developmentSha) || !FULL_SHA.test(input.remoteRefs.checkpointSha) ||
    input.remoteRefs.developmentSha !== cycle.candidate_sha ||
    input.remoteRefs.checkpointSha !== disposition.retry_checkpoint.sha
  ) {
    throw new Error("Sentinel retained-checkpoint manual reconciliation has no exact durable remote refs");
  }
};

const parseNativeReviewExhaustedManualCheckpointReconciliationReports = (
  input: Pick<
    NativeReviewExhaustedManualCheckpointReconciliationInput,
    "workflowRunId" | "workflowRunAttempt" | "workflowFailed" | "selection" | "cycleValue" | "dispositionValue"
  >,
): Readonly<{
  disposition: ReturnType<typeof parseGitHubIssueManualRequiredReport>;
  cycle: ReturnType<typeof parseSentinelManualRequiredCycleReport>;
}> => {
  const disposition = parseGitHubIssueManualRequiredReport(input.dispositionValue, {
    issueId: input.selection.issue_id,
    issueNumber: input.selection.issue_number,
    fingerprint: input.selection.fingerprint,
  });
  const cycle = input.workflowFailed
    ? parseSentinelManualRequiredCycleReport(input.cycleValue, {
      runId: input.workflowRunId,
      runAttempt: input.workflowRunAttempt,
      status: "failed",
      stage: "failed",
      branchDispositions: ["atomic_manual_push_requires_reconciliation"],
    })
    : parseSentinelManualRequiredCycleReport(input.cycleValue, {
      runId: input.workflowRunId,
      runAttempt: input.workflowRunAttempt,
      status: "no_change",
      stage: "complete",
      branchDispositions: ["remote_retained_issue_manual_required"],
    });
  return { disposition, cycle };
};

/**
 * A round-three review failure is successful only when both remote refs are
 * exact, the ledger contains the same immutable candidate, and no delivery
 * artifact exists. It is never an automatic retry or an ordinary PR result.
 */
export const validateNativeReviewExhaustedManualCheckpointReconciliation = (
  input: NativeReviewExhaustedManualCheckpointReconciliationInput,
): void => {
  if (input.pullRequestReportPresent || input.productionOutcomeReportPresent) {
    throw new Error("A native-review-exhausted reconciliation cannot contain delivery or production records");
  }
  const { disposition, cycle } = parseNativeReviewExhaustedManualCheckpointReconciliationReports(input);
  if (
    cycle.candidate_sha === cycle.base_development_sha ||
    JSON.stringify(cycle.retry_checkpoint) !== JSON.stringify(disposition.retry_checkpoint)
  ) {
    throw new Error("Sentinel native-review-exhausted reconciliation has no exact checkpoint receipt");
  }
  const matches = parseGitHubIssueJobLedger(input.developmentLedgerMarkdown).filter((entry) =>
    entry.issueId === input.selection.issue_id && entry.number === input.selection.issue_number &&
    entry.fingerprint === input.selection.fingerprint
  );
  const expectedCheckpoint = {
    branch: disposition.retry_checkpoint.branch,
    sha: disposition.retry_checkpoint.sha,
    baseSha: disposition.retry_checkpoint.base_sha,
  };
  if (
    matches.length !== 1 || matches[0]!.bodySha256 !== input.selection.body_sha256 ||
    matches[0]!.comments !== input.selection.comments ||
    matches[0]!.sourceUpdatedAt !== input.selection.updated_at ||
    matches[0]!.disposition !== "manual_required" ||
    matches[0]!.baseSha !== expectedCheckpoint.baseSha ||
    JSON.stringify(matches[0]!.checkpoint) !== JSON.stringify(expectedCheckpoint) ||
    Date.parse(matches[0]!.recordedAt) < Date.parse(cycle.started_at)
  ) {
    throw new Error("Sentinel native-review-exhausted reconciliation has no exact durable ledger row");
  }
  if (
    !FULL_SHA.test(input.remoteRefs.developmentSha) || !FULL_SHA.test(input.remoteRefs.checkpointSha) ||
    input.remoteRefs.developmentSha !== cycle.candidate_sha ||
    input.remoteRefs.checkpointSha !== expectedCheckpoint.sha
  ) {
    throw new Error("Sentinel native-review-exhausted reconciliation has no exact durable remote refs");
  }
};

export const requireCurrentOpenManualIssueSnapshot = async (
  input: Readonly<{
    token: string;
    repository: string;
    selection: GitHubIssueSelectionReport;
    fetcher?: typeof fetch;
  }>,
): Promise<void> => {
  const currentIssue = await getCurrentGitHubIssueJob(
    new GitHubActionsClient({ repository: input.repository, token: input.token, fetcher: input.fetcher }),
    input.repository,
    input.selection.issue_number,
  );
  if (
    !currentIssue || currentIssue.issueId !== input.selection.issue_id ||
    currentIssue.number !== input.selection.issue_number || currentIssue.fingerprint !== input.selection.fingerprint ||
    currentIssue.bodySha256 !== input.selection.body_sha256 || currentIssue.comments !== input.selection.comments ||
    currentIssue.updatedAt !== input.selection.updated_at
  ) {
    throw new Error("Sentinel manual reconciliation issue snapshot changed or is no longer open");
  }
};

export const reconcileGitHubIssueDelivery = async (
  input: Readonly<{
    repositoryRoot: string;
    token: string;
    repository: string;
    workflowRunId: string;
    workflowRunAttempt: number;
    serverUrl: string;
    workflowFailed: boolean;
    fetcher?: typeof fetch;
  }>,
): Promise<void> => {
  const reportsDir = `${input.repositoryRoot}/.sentinel/reports`;
  const selectionValue = await optionalJson(`${reportsDir}/github-issue-selection.json`);
  if (selectionValue === null) return;
  const selection = parseGitHubIssueSelectionReportAny(selectionValue);
  const cycleValue = await readJson(`${reportsDir}/cycle.json`);
  const cycle = parseSentinelCycleReport(cycleValue);
  if (selection.schema_version === 2) {
    const triageFile = await optionalJson(`${reportsDir}/triage.json`);
    const recoveryValue = await optionalJson(`${reportsDir}/recovery-record-v1.json`);
    if (triageFile === null || recoveryValue === null) {
      throw new Error("Missing V2 frozen plan evidence before issue delivery reconcile");
    }
    const recovery = parseSentinelRecoveryRecord(recoveryValue);
    await verifyFrozenIssuePlanDigest({
      repository: input.repository,
      selection,
      triageValue: triageFile,
      cycleBaseSha: cycle.base_development_sha,
      recovery,
      runId: cycle.run_id,
    });
  }
  const dispositionValue = await optionalJson(`${reportsDir}/github-issue-disposition.json`);
  const dispositionRecord = record(dispositionValue);
  if (dispositionRecord?.disposition === "retry_pending") {
    const pullRequestReportPresent = await regularFileExists(`${reportsDir}/github-issue-pull-request.json`);
    const productionOutcomeReportPresent = await regularFileExists(
      `${reportsDir}/github-issue-production-outcome.json`,
    );
    const ledger = await developmentIssueLedger(input.token, input.repository);
    let remoteRefs: RetryPendingRemoteRefs | undefined;
    if (input.workflowFailed) {
      const { disposition } = parseRetryPendingIssueReconciliationReports({
        workflowRunId: input.workflowRunId,
        workflowFailed: input.workflowFailed,
        selection,
        cycleValue,
        dispositionValue,
        pullRequestReportPresent,
        productionOutcomeReportPresent,
        developmentLedgerMarkdown: ledger.markdown,
      });
      if (disposition.retry_checkpoint === null) {
        throw new Error("A failed atomic retry push reconciliation requires a durable retry checkpoint");
      }
      const [developmentSha, checkpointSha] = await Promise.all([
        readGitHubCommitRefSha(input.token, input.repository, "development"),
        readGitHubCommitRefSha(input.token, input.repository, disposition.retry_checkpoint.branch),
      ]);
      remoteRefs = { developmentSha, checkpointSha };
    }
    validateRetryPendingIssueReconciliation({
      workflowRunId: input.workflowRunId,
      workflowFailed: input.workflowFailed,
      selection,
      cycleValue,
      dispositionValue,
      pullRequestReportPresent,
      productionOutcomeReportPresent,
      developmentLedgerMarkdown: ledger.markdown,
      remoteRefs,
    });
    console.log(
      `[sentinel] issue_retry_pending=#${selection.issue_number} reconciliation=no_delivery issue=open`,
    );
    return;
  }
  if (
    dispositionRecord?.disposition === "manual_required" &&
    dispositionRecord.phase === "retry_checkpoint_resume_failed"
  ) {
    const pullRequestReportPresent = await regularFileExists(`${reportsDir}/github-issue-pull-request.json`);
    const productionOutcomeReportPresent = await regularFileExists(
      `${reportsDir}/github-issue-production-outcome.json`,
    );
    const ledger = await developmentIssueLedger(input.token, input.repository, input.fetcher);
    const { disposition } = parseManualRequiredRetainedCheckpointReconciliationReports({
      workflowRunId: input.workflowRunId,
      selection,
      cycleValue,
      dispositionValue,
    });
    const [developmentSha, checkpointSha] = await Promise.all([
      readGitHubCommitRefSha(input.token, input.repository, "development", input.fetcher),
      readGitHubCommitRefSha(input.token, input.repository, disposition.retry_checkpoint.branch, input.fetcher),
    ]);
    validateManualRequiredRetainedCheckpointReconciliation({
      workflowRunId: input.workflowRunId,
      selection,
      cycleValue,
      dispositionValue,
      pullRequestReportPresent,
      productionOutcomeReportPresent,
      developmentLedgerMarkdown: ledger.markdown,
      remoteRefs: { developmentSha, checkpointSha },
    });
    await requireCurrentOpenManualIssueSnapshot({
      token: input.token,
      repository: input.repository,
      selection,
      fetcher: input.fetcher,
    });
    console.log(
      `[sentinel] issue_manual_required=#${selection.issue_number} reconciliation=no_delivery issue=open`,
    );
    return;
  }
  if (
    dispositionRecord?.disposition === "manual_required" &&
    dispositionRecord.phase === "native_review_exhausted"
  ) {
    const pullRequestReportPresent = await regularFileExists(`${reportsDir}/github-issue-pull-request.json`);
    const productionOutcomeReportPresent = await regularFileExists(
      `${reportsDir}/github-issue-production-outcome.json`,
    );
    const ledger = await developmentIssueLedger(input.token, input.repository, input.fetcher);
    const { disposition } = parseNativeReviewExhaustedManualCheckpointReconciliationReports({
      workflowRunId: input.workflowRunId,
      workflowRunAttempt: input.workflowRunAttempt,
      workflowFailed: input.workflowFailed,
      selection,
      cycleValue,
      dispositionValue,
    });
    const [developmentSha, checkpointSha] = await Promise.all([
      readGitHubCommitRefSha(input.token, input.repository, "development", input.fetcher),
      readGitHubCommitRefSha(input.token, input.repository, disposition.retry_checkpoint.branch, input.fetcher),
    ]);
    validateNativeReviewExhaustedManualCheckpointReconciliation({
      workflowRunId: input.workflowRunId,
      workflowRunAttempt: input.workflowRunAttempt,
      workflowFailed: input.workflowFailed,
      selection,
      cycleValue,
      dispositionValue,
      pullRequestReportPresent,
      productionOutcomeReportPresent,
      developmentLedgerMarkdown: ledger.markdown,
      remoteRefs: { developmentSha, checkpointSha },
    });
    await requireCurrentOpenManualIssueSnapshot({
      token: input.token,
      repository: input.repository,
      selection,
      fetcher: input.fetcher,
    });
    console.log(
      `[sentinel] issue_manual_required=#${selection.issue_number} reconciliation=no_delivery issue=open`,
    );
    return;
  }
  const pullValue = await optionalJson(`${reportsDir}/github-issue-pull-request.json`);
  if (pullValue === null) {
    if (input.workflowFailed) return;
    throw new Error("A completed Sentinel issue cycle has no pull-request delivery record");
  }
  const pullRecord = parseGitHubIssuePullRequestRecord(pullValue);
  if (
    pullRecord.issue_number !== selection.issue_number ||
    pullRecord.fingerprint !== selection.fingerprint
  ) {
    throw new Error("Sentinel issue pull-request record does not match the selected issue snapshot");
  }

  const marker = issueEvidenceMarker(selection);
  const disposition = parseDisposition(dispositionValue);
  const outcome = parseOutcome(await optionalJson(`${reportsDir}/github-issue-production-outcome.json`));
  if (outcome && outcome.candidateSha !== pullRecord.head_sha) {
    await validateMatrixIssueDeliveryMerge({
      token: input.token,
      repository: input.repository,
      workflowRunId: input.workflowRunId,
      workflowRunAttempt: input.workflowRunAttempt,
      pullRequest: pullRecord,
      outcome: outcome.outcome,
      deployedSha: outcome.candidateSha,
      matrixPlanValue: await optionalJson(`${reportsDir}/matrix-plan.json`),
      matrixCycleValue: await optionalJson(`${reportsDir}/matrix-cycle.json`),
      fetcher: input.fetcher,
    });
  }
  const durableEvidence = await completionEvidence(
    input.token,
    input.repository,
    selection.issue_number,
    marker,
  );
  if (durableEvidence !== null) {
    if (input.workflowFailed || disposition !== "resolved" || outcome?.outcome !== "kept") {
      throw new Error("Durable completion evidence cannot override the current workflow disposition");
    }
    const source = new GitHubActionsClient({ repository: input.repository, token: input.token });
    let issue = await source.getIssue(selection.issue_number);
    const requireMatchingOpenSnapshot = async (): Promise<void> => {
      if (
        !await completionEvidenceSnapshotMatches(
          source,
          input.repository,
          selection,
          issue,
          durableEvidence,
        )
      ) {
        throw new Error("Sentinel completion evidence no longer matches the open issue snapshot");
      }
    };
    if (issue.state === "open") await requireMatchingOpenSnapshot();
    const merge = await mergeDeliveryPullRequest(input.token, input.repository, pullRecord);
    // The merge can take long enough for the issue body, metadata, comments,
    // or relationships to change. Fetch and validate the full snapshot again
    // immediately before the irreversible close.
    issue = await source.getIssue(selection.issue_number);
    if (issue.state === "open") {
      await closeIssueAfterCompletionEvidenceRevalidation(
        source,
        input.repository,
        selection,
        durableEvidence,
        () => closeIssue(input.token, input.repository, selection.issue_number),
      );
    } else {
      const issueState = await getIssueState(
        input.token,
        input.repository,
        selection.issue_number,
      );
      if (issueState.stateReason !== "completed") {
        throw new Error("Sentinel completion evidence exists on an issue closed for a different reason");
      }
    }
    await upsertComment(
      input.token,
      input.repository,
      pullRecord.pull_request_number,
      marker,
      durableEvidence.body,
    );
    await writeReconciliationReport(reportsDir, {
      issueNumber: selection.issue_number,
      fingerprint: selection.fingerprint,
      pullRequestNumber: pullRecord.pull_request_number,
      pullRequestMerged: true,
      mergeSource: merge.source,
      action: "close_completed",
      issueSnapshotMatches: true,
      durableCompletionEvidenceReused: true,
    });
    return;
  }

  let pullMerged = false;
  let mergeSource: PullRequestMergeSource = null;
  if (!input.workflowFailed && disposition === "resolved" && outcome?.outcome === "kept") {
    try {
      const merge = await mergeDeliveryPullRequest(input.token, input.repository, pullRecord);
      pullMerged = true;
      mergeSource = merge.source;
    } catch (error) {
      // A kept delivery whose pull request cannot be merged stays open with
      // evidence; the snapshot-based recheck below leaves the issue open too.
      console.warn(`[sentinel] issue_pull_request_merge=#${pullRecord.pull_request_number} failed`, error);
    }
  }

  let issueSnapshotMatches = false;
  if (
    !input.workflowFailed && disposition === "resolved" && outcome?.outcome === "kept" &&
    pullMerged
  ) {
    const current = await getCurrentGitHubIssueJob(
      new GitHubActionsClient({ repository: input.repository, token: input.token }),
      input.repository,
      selection.issue_number,
    );
    issueSnapshotMatches = current !== null && current.issueId === selection.issue_id &&
      current.fingerprint === selection.fingerprint && current.bodySha256 === selection.body_sha256 &&
      current.updatedAt === selection.updated_at;
  }
  const action = evaluateIssueCompletionAction({
    hasSelection: true,
    workflowFailed: input.workflowFailed,
    disposition,
    outcome: outcome?.outcome ?? null,
    pullRequestMerged: pullMerged,
    issueSnapshotMatches,
  });
  const workflowEvidence = await productionWorkflowEvidence(reportsDir);
  const workflowRunUrl = `${input.serverUrl}/${input.repository}/actions/runs/${input.workflowRunId}`;
  const evidence = renderIssueDeliveryEvidence({
    repository: input.repository,
    selection,
    pullRequest: pullRecord,
    workflowRunUrl,
    action,
    cycleStatus: cycle.status,
    candidateSha: pullRecord.head_sha,
    productionRevision: outcome?.candidateRevision ?? null,
    deploymentWorkflowRunId: workflowEvidence.deploymentRunId,
    promotionWorkflowRunId: workflowEvidence.promotionRunId,
    monitoringDecision: workflowEvidence.monitoringDecision,
  });

  await upsertComment(
    input.token,
    input.repository,
    pullRecord.pull_request_number,
    marker,
    evidence,
  );
  if (action === "close_completed") {
    // The issue evidence is the durable retry checkpoint. Persist it before the irreversible close.
    await upsertComment(
      input.token,
      input.repository,
      selection.issue_number,
      marker,
      evidence,
    );
    const durableEvidence = await completionEvidence(
      input.token,
      input.repository,
      selection.issue_number,
      marker,
    );
    if (durableEvidence === null) {
      throw new Error("Sentinel completion evidence was not durable before issue closure");
    }
    await closeIssueAfterCompletionEvidenceRevalidation(
      new GitHubActionsClient({ repository: input.repository, token: input.token }),
      input.repository,
      selection,
      durableEvidence,
      () => closeIssue(input.token, input.repository, selection.issue_number),
    );
  } else if (action === "leave_open_rolled_back") {
    await removeRolledBackLedgerEntry(
      input.token,
      input.repository,
      selection.issue_number,
      selection.fingerprint,
    );
  }

  await writeReconciliationReport(reportsDir, {
    issueNumber: selection.issue_number,
    fingerprint: selection.fingerprint,
    pullRequestNumber: pullRecord.pull_request_number,
    pullRequestMerged: pullMerged,
    mergeSource,
    action,
    issueSnapshotMatches,
    durableCompletionEvidenceReused: false,
  });
};

if (import.meta.main) {
  const repositoryRoot = Deno.cwd();
  const workflowOutcome = Deno.env.get("SENTINEL_WORKFLOW_OUTCOME")?.trim();
  const workflowRunAttempt = Number(requiredEnvironment("GITHUB_RUN_ATTEMPT"));
  if (!Number.isSafeInteger(workflowRunAttempt) || workflowRunAttempt <= 0) {
    throw new Error("GITHUB_RUN_ATTEMPT must be a positive integer for Sentinel issue reconciliation");
  }
  await reconcileGitHubIssueDelivery({
    repositoryRoot,
    token: requiredEnvironment("GITHUB_TOKEN"),
    repository: requiredEnvironment("GITHUB_REPOSITORY"),
    workflowRunId: requiredEnvironment("GITHUB_RUN_ID"),
    workflowRunAttempt,
    serverUrl: Deno.env.get("GITHUB_SERVER_URL")?.trim() || "https://github.com",
    workflowFailed: workflowOutcome === "failure" || workflowOutcome === "cancelled" ||
      workflowOutcome === "timed_out",
  });
}
