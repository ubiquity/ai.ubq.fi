import type { GitHubIssue, GitHubIssueComment, GitHubIssueRelations, GitHubRepositoryPermission } from "./github.ts";
import { isSentinelProtectedImplementationPath, SENTINEL_POLICY } from "./policy.ts";
import { matrixAllowedPathCovers } from "./matrix.ts";
import {
  parseSentinelRecoveryLedger,
  resolveSentinelRecoverySelection,
  type SentinelRecoveryEligibilityContext,
} from "./recovery-ledger.ts";
import type { NativeReviewFinding, NativeReviewReport, SentinelInterval, TriageReport } from "./types.ts";

export interface GitHubIssueJobSource {
  listOpenIssues(): Promise<readonly GitHubIssue[]>;
  getIssue(issueNumber: number): Promise<GitHubIssue>;
  listIssueComments(issueNumber: number): Promise<readonly GitHubIssueComment[]>;
  getIssueRelations(issueNumber: number): Promise<GitHubIssueRelations>;
  getRepositoryPermission(username: string): Promise<GitHubRepositoryPermission>;
}

export type GitHubIssueIntake = "declared" | "owner_backlog" | "backlog";

export type GitHubIssueJob = Readonly<{
  repository: string;
  issueId: number;
  nodeId: string;
  number: number;
  htmlUrl: string;
  title: string;
  body: string;
  bodySha256: string;
  fingerprint: string;
  /** Execution severity adapter, distinct from queue order (numeric >=3 is P2). */
  priority: "P2" | "P3";
  /** The highest recognized numeric priority label, or "" when absent/unrecognized. */
  priorityLabel: string;
  /** Recognized nonnegative numeric queue priority; null when absent/unrecognized. */
  queuePriority: number | null;
  /** True when the issue carries several recognized numeric priority labels. */
  queuePriorityAmbiguous: boolean;
  timeLabel: string | null;
  intake: GitHubIssueIntake;
  labels: readonly string[];
  files: readonly string[];
  acceptance: readonly string[];
  /** Canonical digest of material comment/dependency context; null for legacy projections. */
  materialDigest: string | null;
  /** Canonical captured ordinary discussion that the material digest binds; planning data. */
  capturedComments: readonly GitHubIssueComment[];
  authorLogin: string;
  authorAssociation: string;
  comments: number;
  createdAt: string;
  updatedAt: string;
  relations: GitHubIssueRelations;
}>;

/** One inspected candidate's explicit queue disposition. */
export type GitHubIssueQueueEntry = Readonly<{
  number: number;
  node_id: string;
  fingerprint: string;
  queue_priority: number | null;
  selected: boolean;
  reason: string;
  next_action: string;
}>;

export type GitHubIssueQueueDispositions = Readonly<{
  queue_exhausted: boolean;
  entries: readonly GitHubIssueQueueEntry[];
}>;

export type GitHubIssueJobDisposition =
  | "retry_pending"
  | "checkpoint_retained"
  | "resolved"
  | "manual_required";

export type GitHubIssueJobCheckpoint = Readonly<{
  branch: string;
  sha: string;
  baseSha: string;
}>;

/**
 * A model report is metadata only. Keep the Git-derived paths on a mismatch
 * so the caller can checkpoint the candidate before it records the failure.
 */
export class SentinelChangedFilesMismatchError extends Error {
  readonly actualChangedFiles: readonly string[];
  readonly reportedChangedFiles: readonly string[];
  readonly source: "github_issue" | "review_backlog";

  constructor(
    message: string,
    actualChangedFiles: readonly string[],
    reportedChangedFiles: readonly string[],
    source: "github_issue" | "review_backlog",
  ) {
    super(message);
    this.name = "SentinelChangedFilesMismatchError";
    this.actualChangedFiles = Object.freeze([...actualChangedFiles]);
    this.reportedChangedFiles = Object.freeze([...reportedChangedFiles]);
    this.source = source;
  }
}

export const isSentinelChangedFilesMismatchError = (
  error: unknown,
): error is SentinelChangedFilesMismatchError => error instanceof SentinelChangedFilesMismatchError;

export type GitHubIssueJobPlanning =
  | Readonly<{ ready: true; triage: TriageReport }>
  | Readonly<{ ready: false; reason: string; persisted: boolean }>;

export type GitHubIssueJobPlanningContext = Readonly<{
  checkpoint: GitHubIssueJobCheckpoint | null;
  recovery: SentinelRecoveryEligibilityContext;
}>;

export type GitHubIssueJobPlanningCallback = (
  job: GitHubIssueJob,
  context: GitHubIssueJobPlanningContext,
) => Promise<GitHubIssueJobPlanning>;

export type GitHubIssueJobSelection = Readonly<{
  job: GitHubIssueJob | null;
  checkpoint: GitHubIssueJobCheckpoint | null;
  /** The frozen plan triage when the runtime planning callback produced one. */
  triage: TriageReport | null;
  /** Explicit per-issue queue dispositions for the inspected ordered queue. */
  queue: GitHubIssueQueueDispositions;
}>;

export type GitHubIssueJobHint = Readonly<{
  schema_version: 1;
  selection:
    | Readonly<{
      repository: string;
      issue_id: number;
      node_id: string;
      issue_number: number;
      fingerprint: string;
      checkpoint:
        | Readonly<{
          branch: string;
          sha: string;
          base_sha: string;
        }>
        | null;
    }>
    | null;
}>;

export type GitHubIssueJobLedgerEntry = Readonly<{
  issueId: number;
  nodeId: string;
  number: number;
  fingerprint: string;
  bodySha256: string;
  comments: number;
  sourceUpdatedAt: string;
  recordedAt: string;
  baseSha: string;
  checkpoint: GitHubIssueJobCheckpoint | null;
  title: string;
  disposition: GitHubIssueJobDisposition;
}>;

const PRIORITY_LABELS = new Map<string, GitHubIssueJob["priority"]>([
  ["Priority: 2 (Medium)", "P3"],
  ["Priority: 3 (High)", "P2"],
]);
export const MAX_GITHUB_ISSUE_TIME_ESTIMATE_MINUTES = 24 * 60;
const DEFAULT_OWNER_BACKLOG_PRIORITY_LABEL = "Priority: 2 (Medium)";
const DEFAULT_OWNER_BACKLOG_TIME_LABEL = "Time: <2 Hours";
const TIME_LABEL_PATTERN = /^Time: <([1-9][0-9]*) (Minute|Minutes|Hour|Hours|Day|Days)$/u;
const NUMERIC_PRIORITY_PATTERN = /^Priority: ([0-9]+)(?: \([^)]*\))?$/u;
const FILE_LOCATION_PATTERN = /^([A-Za-z0-9_.@/+\-]+)(?::\d+(?:-\d+)?(?::\d+)?)?$/u;
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const NODE_ID = /^[A-Za-z0-9_=-]{4,160}$/u;
const CHECKPOINT_BRANCH =
  /^sentinel\/candidate-(?:[1-9][0-9]*(?:-[1-9][0-9]*)?|(?:github_issue|review_backlog|triage|incident)-[A-Za-z0-9][A-Za-z0-9._-]{0,79}-[A-Za-z0-9][A-Za-z0-9._-]{0,31}-g[1-9][0-9]*-[0-9a-f]{16})$/u;
const MAX_ISSUE_BODY_BYTES = 32 * 1_024;
const MAX_ISSUE_FILES = 32;
const MAX_ACCEPTANCE_ITEMS = 32;
const MAX_LEDGER_BYTES = 256 * 1_024;
const MAX_LEDGER_ENTRIES = 512;
const MAX_LEDGER_LINE_LENGTH = 4_096;
export const GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS = 6 * 60 * 60 * 1_000;
export const GITHUB_ISSUE_JOB_HINT_FILENAME = "sentinel-github-issue-job-hint.json";
const LEDGER_HEADERS = Object.freeze([
  "Issue",
  "REST ID",
  "Node ID",
  "Fingerprint",
  "Body SHA-256",
  "Comments",
  "Source updated",
  "Recorded",
  "Base SHA",
  "Checkpoint branch",
  "Checkpoint SHA",
  "Title",
  "Disposition",
]);

const MAX_ISSUE_JOB_HINT_BYTES = 1_024;
const record = (value: unknown): Record<string, unknown> | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;
const hasExactKeys = (value: Record<string, unknown>, keys: readonly string[]): boolean => {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  return actual.length === expected.length && actual.every((key, index) => key === expected[index]);
};

const textEncoder = new TextEncoder();

const sha256 = async (value: string): Promise<string> => {
  const digest = await crypto.subtle.digest("SHA-256", textEncoder.encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const validTimestamp = (value: string): boolean =>
  /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/u.test(value) && Number.isFinite(Date.parse(value));

const UBIQUITY_OS_LABEL_DENIAL_COMMENT =
  /^> \[!WARNING\]\n> You are not allowed to set labels\.\n\n<!-- UbiquityOS - updateLabels - [0-9a-f]{64} - @[A-Za-z0-9-]+ - https:\/\/console\.deno\.com\/ubiquity-os\/daemon-pricing\/observability\/logs\?start=\d{4}-\d{2}-\d{2}T\d{2}%3A\d{2}%3A\d{2}Z&end=\d{4}-\d{2}-\d{2}T\d{2}%3A\d{2}%3A\d{2}Z&tz=Etc%2FUTC\n\{\n[ ]{2}"caller": "updateLabels"\n\}\n-->\n$/u;

export const isSentinelInertIssueComment = (comment: GitHubIssueComment): boolean =>
  Number.isSafeInteger(comment.id) && comment.id > 0 &&
  comment.authorLogin === "ubiquity-os[bot]" && comment.authorType === "Bot" &&
  validTimestamp(comment.createdAt) && validTimestamp(comment.updatedAt) &&
  comment.updatedAt === comment.createdAt && typeof comment.body === "string" &&
  UBIQUITY_OS_LABEL_DENIAL_COMMENT.test(comment.body);

const sortedUnique = (values: readonly string[]): string[] | null => {
  if (values.some((value) => value.trim() !== value || value.length === 0)) return null;
  const unique = new Set(values);
  return unique.size === values.length ? [...unique].sort() : null;
};

const validatedRepository = (repository: string): string => {
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u.test(repository)) {
    throw new Error("GitHub issue repository must use the owner/name form");
  }
  return repository;
};

const validIssueUrl = (value: string, repository: string, issueNumber: number): boolean => {
  try {
    const url = new URL(value);
    return url.protocol === "https:" && url.hostname === "github.com" && url.username === "" && url.password === "" &&
      url.search === "" && url.hash === "" && url.pathname === `/${repository}/issues/${issueNumber}`;
  } catch {
    return false;
  }
};

export const parseGitHubIssueTimeLabel = (label: string): number | null => {
  const match = label.match(TIME_LABEL_PATTERN);
  if (!match) return null;
  const value = Number(match[1]);
  const unit = match[2]!;
  if (!Number.isSafeInteger(value) || value <= 0) return null;
  const singular = unit === "Minute" || unit === "Hour" || unit === "Day";
  if ((value === 1) !== singular) return null;
  const multiplier = unit === "Minute" || unit === "Minutes" ? 1 : unit === "Hour" || unit === "Hours" ? 60 : 1_440;
  const minutes = value * multiplier;
  return Number.isSafeInteger(minutes) && minutes <= MAX_GITHUB_ISSUE_TIME_ESTIMATE_MINUTES ? minutes : null;
};

type ParsedIssueBody = Readonly<{ acceptance: readonly string[]; files: readonly string[] }>;

export const parseGitHubIssueJobBody = (body: string): ParsedIssueBody | null => {
  if (textEncoder.encode(body).byteLength > MAX_ISSUE_BODY_BYTES) return null;
  const lines = body.replaceAll("\r\n", "\n").split("\n");
  const acceptanceIndex = lines.indexOf("Acceptance:");
  const filesIndex = lines.indexOf("Files:");
  if (
    acceptanceIndex <= 0 || filesIndex <= acceptanceIndex + 1 ||
    lines.lastIndexOf("Acceptance:") !== acceptanceIndex || lines.lastIndexOf("Files:") !== filesIndex ||
    !lines.slice(0, acceptanceIndex).some((line) => line.trim().length > 0)
  ) return null;

  const acceptanceLines = lines.slice(acceptanceIndex + 1, filesIndex).filter((line) => line.trim().length > 0);
  const fileLines = lines.slice(filesIndex + 1).filter((line) => line.trim().length > 0);
  if (
    acceptanceLines.length === 0 || acceptanceLines.length > MAX_ACCEPTANCE_ITEMS ||
    fileLines.length === 0 || fileLines.length > MAX_ISSUE_FILES ||
    acceptanceLines.some((line) => !line.startsWith("- ")) || fileLines.some((line) => !line.startsWith("- "))
  ) return null;
  const acceptance = acceptanceLines.map((line) => line.slice(2).trim());
  if (acceptance.some((item) => item.length === 0 || item.length > 1_000)) return null;

  const files: string[] = [];
  for (const line of fileLines) {
    const value = line.slice(2).trim();
    const match = value.match(FILE_LOCATION_PATTERN);
    if (!match) return null;
    const path = match[1]!;
    if (isSentinelProtectedImplementationPath(path)) return null;
    files.push(path);
  }
  const canonicalFiles = sortedUnique(files);
  if (!canonicalFiles || canonicalFiles.length === 0) return null;
  return { acceptance, files: canonicalFiles };
};

const parseOwnerBacklogIssueBody = (body: string): ParsedIssueBody | null => {
  if (textEncoder.encode(body).byteLength > MAX_ISSUE_BODY_BYTES) return null;
  const lines = body.replaceAll("\r\n", "\n").split("\n");
  const headings = ["## Context", "## Gap", "## Proposed"];
  if (headings.some((heading) => lines.filter((line) => line === heading).length !== 1)) return null;
  const proposedStart = lines.indexOf("## Proposed") + 1;
  const proposedEnd = lines.findIndex((line, index) => index >= proposedStart && line.startsWith("## "));
  const proposedLines = lines.slice(proposedStart, proposedEnd === -1 ? undefined : proposedEnd);
  if (!proposedLines.some((line) => line.trim().length > 0)) return null;

  const files: string[] = [];
  for (const match of body.matchAll(/`([^`\r\n]+)`/gu)) {
    const pathMatch = match[1]!.trim().match(FILE_LOCATION_PATTERN);
    if (!pathMatch) continue;
    const path = pathMatch[1]!;
    if (isSentinelProtectedImplementationPath(path)) return null;
    if (path.startsWith("src/")) files.push(path);
  }
  const canonicalFiles = sortedUnique([...new Set(files)]);
  if (!canonicalFiles || canonicalFiles.length === 0 || canonicalFiles.length > MAX_ISSUE_FILES) return null;
  return {
    acceptance: ["Implement the owner-authored proposal within the extracted source-file scope."],
    files: canonicalFiles,
  };
};

const issueJobLabels = (
  labels: readonly string[],
  maximumTimeEstimateMinutes: number,
):
  | Readonly<{
    labels: readonly string[];
    priority: GitHubIssueJob["priority"];
    priorityLabel: GitHubIssueJob["priorityLabel"];
    timeLabel: string;
  }>
  | null => {
  if (
    !Number.isSafeInteger(maximumTimeEstimateMinutes) || maximumTimeEstimateMinutes <= 0 ||
    maximumTimeEstimateMinutes > MAX_GITHUB_ISSUE_TIME_ESTIMATE_MINUTES
  ) throw new Error("GitHub issue time-estimate policy is invalid");
  const canonicalLabels = sortedUnique(labels);
  if (!canonicalLabels) return null;
  const priorityLabels = canonicalLabels.filter((label) => label.startsWith("Priority: "));
  const timeLabels = canonicalLabels.filter((label) => label.startsWith("Time: "));
  if (priorityLabels.length !== 1 || timeLabels.length !== 1) return null;
  const priorityLabel = priorityLabels[0] as GitHubIssueJob["priorityLabel"];
  const timeLabel = timeLabels[0]!;
  const priority = PRIORITY_LABELS.get(priorityLabel);
  const timeEstimateMinutes = parseGitHubIssueTimeLabel(timeLabel);
  if (!priority || timeEstimateMinutes === null || timeEstimateMinutes > maximumTimeEstimateMinutes) return null;
  return { labels: canonicalLabels, priority, priorityLabel, timeLabel };
};

const baseIssueStructuralEligible = (issue: GitHubIssue): boolean =>
  issue.state === "open" && !issue.isPullRequest && issue.title.length <= 256 &&
  Number.isSafeInteger(issue.id) && issue.id > 0 && Number.isSafeInteger(issue.number) && issue.number > 0 &&
  Number.isSafeInteger(issue.comments) && issue.comments >= 0;

/** Highest recognized numeric repository priority; null when absent/unrecognized. */
export const parseNumericQueuePriority = (labels: readonly string[]): number | null => {
  let highest: number | null = null;
  for (const label of labels) {
    const match = label.match(NUMERIC_PRIORITY_PATTERN);
    if (!match) continue;
    const value = Number(match[1]!);
    if (!Number.isSafeInteger(value) || value < 0) continue;
    if (highest === null || value > highest) highest = value;
  }
  return highest;
};

const queuePriorityIsAmbiguous = (labels: readonly string[]): boolean => {
  const recognized = labels.filter((label) => NUMERIC_PRIORITY_PATTERN.test(label)).length;
  return recognized > 1;
};

const recognizedQueuePriorityLabel = (labels: readonly string[]): string => {
  const highest = parseNumericQueuePriority(labels);
  if (highest === null) return "";
  const withHighest = labels.filter(
    (label) => NUMERIC_PRIORITY_PATTERN.test(label) && Number(label.match(NUMERIC_PRIORITY_PATTERN)![1]!) === highest,
  );
  return withHighest.sort()[0] ?? "";
};

/** Legacy projection serialization only: defaults for unchanged supported old sources. */
const relationsProjection = (relations: GitHubIssueRelations): Readonly<Record<string, unknown>> => ({
  parentIssueNumber: relations.parentIssueNumber,
  subIssueCount: relations.subIssueCount,
  blockedByCount: relations.blockedByCount,
  blockingCount: relations.blockingCount,
  latestBodyEdit: relations.latestBodyEdit,
  latestTitleEdit: relations.latestTitleEdit,
});

/**
 * Canonical digest of the material discussion/dependency context: ordinary
 * comments (content, author, timestamps), plus incoming prerequisite
 * identities and states. Inert UbiquityOS label-denial notices are excluded;
 * legacy projections with no material context keep digest null.
 */
export const materialContextDigest = async (
  issue: GitHubIssue,
  comments: readonly GitHubIssueComment[],
  relations: GitHubIssueRelations,
): Promise<string | null> => {
  const ids = new Set<number>();
  for (const comment of comments) {
    if (!Number.isSafeInteger(comment.id) || comment.id <= 0 || ids.has(comment.id)) {
      throw new Error("GitHub issue comment capture is incomplete or duplicated");
    }
    ids.add(comment.id);
  }
  if (ids.size !== issue.comments) {
    throw new Error("GitHub issue comment capture does not match the issue comment count");
  }
  const material = comments
    .filter((comment) => !isSentinelInertIssueComment(comment))
    .map((comment) => ({
      id: comment.id,
      author_login: comment.authorLogin,
      author_type: comment.authorType,
      body: comment.body,
      created_at: comment.createdAt,
      updated_at: comment.updatedAt,
    }))
    .sort((left, right) => left.id - right.id);
  const dependencies = (relations.dependencies ?? [])
    .map((dependency) => ({
      issue_number: dependency.issue_number,
      state: dependency.state,
    }))
    .sort((left, right) => left.issue_number - right.issue_number);
  if (material.length === 0 && dependencies.length === 0) return null;
  return await sha256(JSON.stringify({ comments: material, dependencies }));
};

const createGitHubIssueJobInner = async (
  repository: string,
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
  materialDigest: string | null,
  capturedComments: readonly GitHubIssueComment[],
): Promise<GitHubIssueJob | null> => {
  validatedRepository(repository);
  if (
    !baseIssueStructuralEligible(issue) || !validTimestamp(issue.createdAt) || !validTimestamp(issue.updatedAt) ||
    Date.parse(issue.updatedAt) < Date.parse(issue.createdAt) || !validIssueUrl(issue.htmlUrl, repository, issue.number)
  ) return null;
  // Source consistency: a stale title timeline is an inconsistent optimistic
  // capture and fails closed explicitly, never silently admitted.
  if (relations.latestTitleEdit !== null && relations.latestTitleEdit.title !== issue.title) {
    throw new Error(`GitHub issue ${issue.number} source is inconsistent: title edit disagrees with the current title`);
  }
  // Legacy source-identity projections are derived deterministically from the
  // source alone: an unchanged previously supported snapshot keeps its exact
  // fingerprint; any other open source is admitted as broad `backlog` intake
  // (empty source hints mean planning required).
  const parsedBody = parseGitHubIssueJobBody(issue.body);
  const parsedLabels = issueJobLabels(issue.labels, MAX_GITHUB_ISSUE_TIME_ESTIMATE_MINUTES);
  const legacyDeclared = parsedBody !== null && parsedLabels !== null
    ? { body: parsedBody, labels: parsedLabels, intake: "declared" as const }
    : null;
  const legacyOwnerBacklog = legacyDeclared === null && issue.labels.length === 0
    ? (() => {
      const ownerBacklogBody = parseOwnerBacklogIssueBody(issue.body);
      return ownerBacklogBody === null ? null : {
        body: ownerBacklogBody,
        labels: {
          labels: [] as const,
          priority: "P3" as const,
          priorityLabel: DEFAULT_OWNER_BACKLOG_PRIORITY_LABEL,
          timeLabel: DEFAULT_OWNER_BACKLOG_TIME_LABEL,
        },
        intake: "owner_backlog" as const,
      };
    })()
    : null;
  const queuePriority = parseNumericQueuePriority(issue.labels);
  const queuePriorityLabel = recognizedQueuePriorityLabel(issue.labels);
  const queuePriorityAmbiguousFlag = queuePriorityIsAmbiguous(issue.labels);
  const canonicalLabels = sortedUnique(issue.labels) ?? issue.labels;
  const legacyProjection = legacyDeclared ?? legacyOwnerBacklog;
  const selectedIntake = legacyProjection ?? {
    body: {
      acceptance: [] as const,
      files: [] as const,
    },
    labels: {
      labels: canonicalLabels,
      priority: (queuePriority !== null && queuePriority >= 3 ? "P2" : "P3") as GitHubIssueJob["priority"],
      priorityLabel: queuePriorityLabel,
      timeLabel: null,
    },
    intake: "backlog" as const,
  };
  const { body: effectiveBody, intake, labels: effectiveLabels } = selectedIntake;
  const bodySha256 = await sha256(issue.body);
  const fingerprint = await sha256(JSON.stringify({
    schema_version: 1,
    repository,
    issue_id: issue.id,
    node_id: issue.nodeId,
    number: issue.number,
    state: issue.state,
    title: issue.title,
    body_sha256: bodySha256,
    html_url: issue.htmlUrl,
    author_login: issue.authorLogin,
    author_association: issue.authorAssociation,
    labels: effectiveLabels.labels,
    assignees: issue.assignees,
    locked: issue.locked,
    comments: issue.comments,
    created_at: issue.createdAt,
    updated_at: issue.updatedAt,
    relations: relationsProjection(relations),
    // Existing declared snapshots must retain their ledger fingerprint; the
    // broad intake marker appears only for newly admitted backlog sources.
    ...(intake === "owner_backlog" ? { intake } : intake === "backlog" ? { intake } : {}),
    files: effectiveBody.files,
    acceptance: effectiveBody.acceptance,
    ...(materialDigest !== null ? { material_digest: materialDigest } : {}),
  }));
  return {
    repository,
    issueId: issue.id,
    nodeId: issue.nodeId,
    number: issue.number,
    htmlUrl: issue.htmlUrl,
    title: issue.title,
    body: issue.body,
    bodySha256,
    fingerprint,
    priority: effectiveLabels.priority,
    priorityLabel: effectiveLabels.priorityLabel,
    queuePriority,
    queuePriorityAmbiguous: queuePriorityAmbiguousFlag,
    timeLabel: effectiveLabels.timeLabel,
    intake,
    labels: effectiveLabels.labels,
    files: effectiveBody.files,
    acceptance: effectiveBody.acceptance,
    materialDigest,
    capturedComments: Object.freeze([...capturedComments].sort((left, right) => left.id - right.id)),
    authorLogin: issue.authorLogin,
    authorAssociation: issue.authorAssociation,
    comments: issue.comments,
    createdAt: issue.createdAt,
    updatedAt: issue.updatedAt,
    relations,
  };
};

export const createGitHubIssueJob = async (
  repository: string,
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
  capturedComments: readonly GitHubIssueComment[] = [],
): Promise<GitHubIssueJob | null> => {
  // The material digest validates capture completeness once and binds the
  // actual ordinary discussion (plus dependency identities) into the identity.
  const materialDigest = await materialContextDigest(issue, capturedComments, relations);
  return await createGitHubIssueJobInner(repository, issue, relations, materialDigest, capturedComments);
};

/**
 * Historical identity projection for a proven inert-notice-only context: the
 * old ledger row serialized count/timestamps without material comment content.
 * The caller must have proven the current capture is inert-only; ordinary
 * material discussion is never normalized away through this path.
 */
export const createGitHubIssueJobHistoricalProjection = async (
  repository: string,
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
): Promise<GitHubIssueJob | null> => await createGitHubIssueJobInner(repository, issue, relations, null, []);

export const githubIssueJobsMatch = (expected: GitHubIssueJob, actual: GitHubIssueJob | null): boolean =>
  actual !== null && expected.repository === actual.repository && expected.issueId === actual.issueId &&
  expected.nodeId === actual.nodeId && expected.number === actual.number && expected.fingerprint === actual.fingerprint;

export const renderGitHubIssueJobHint = (
  job: GitHubIssueJob | null,
  checkpoint: GitHubIssueJobCheckpoint | null = null,
): string =>
  `${
    JSON.stringify(
      {
        schema_version: 1,
        selection: job
          ? {
            repository: job.repository,
            issue_id: job.issueId,
            node_id: job.nodeId,
            issue_number: job.number,
            fingerprint: job.fingerprint,
            checkpoint: checkpoint
              ? {
                branch: checkpoint.branch,
                sha: checkpoint.sha,
                base_sha: checkpoint.baseSha,
              }
              : null,
          }
          : null,
      } satisfies GitHubIssueJobHint,
    )
  }\n`;

export const parseGitHubIssueJobHint = (value: string): GitHubIssueJobHint => {
  if (textEncoder.encode(value).byteLength > MAX_ISSUE_JOB_HINT_BYTES) {
    throw new Error("Sentinel GitHub issue-job hint exceeds its byte limit");
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(value);
  } catch {
    throw new Error("Sentinel GitHub issue-job hint is not valid JSON");
  }
  const hint = record(parsed);
  if (!hint || !hasExactKeys(hint, ["schema_version", "selection"]) || hint.schema_version !== 1) {
    throw new Error("Sentinel GitHub issue-job hint has an invalid envelope");
  }
  if (hint.selection === null) return { schema_version: 1, selection: null };
  const selection = record(hint.selection);
  if (
    !selection ||
    !hasExactKeys(selection, ["repository", "issue_id", "node_id", "issue_number", "fingerprint", "checkpoint"]) ||
    typeof selection.repository !== "string" || validatedRepository(selection.repository) !== selection.repository ||
    !Number.isSafeInteger(selection.issue_id) || (selection.issue_id as number) <= 0 ||
    typeof selection.node_id !== "string" || !NODE_ID.test(selection.node_id) ||
    !Number.isSafeInteger(selection.issue_number) || (selection.issue_number as number) <= 0 ||
    typeof selection.fingerprint !== "string" || !SHA256.test(selection.fingerprint)
  ) throw new Error("Sentinel GitHub issue-job hint has an invalid selection");
  const checkpoint = selection.checkpoint === null ? null : record(selection.checkpoint);
  if (
    selection.checkpoint !== null &&
    (!checkpoint || !hasExactKeys(checkpoint, ["branch", "sha", "base_sha"]) ||
      typeof checkpoint.branch !== "string" || !CHECKPOINT_BRANCH.test(checkpoint.branch) ||
      typeof checkpoint.sha !== "string" || !FULL_SHA.test(checkpoint.sha) ||
      typeof checkpoint.base_sha !== "string" || !FULL_SHA.test(checkpoint.base_sha) ||
      checkpoint.sha === checkpoint.base_sha)
  ) throw new Error("Sentinel GitHub issue-job hint has an invalid checkpoint");
  return {
    schema_version: 1,
    selection: {
      repository: selection.repository,
      issue_id: selection.issue_id as number,
      node_id: selection.node_id,
      issue_number: selection.issue_number as number,
      fingerprint: selection.fingerprint,
      checkpoint: checkpoint
        ? {
          branch: checkpoint.branch as string,
          sha: checkpoint.sha as string,
          base_sha: checkpoint.base_sha as string,
        }
        : null,
    },
  };
};

export const githubIssueJobMatchesHint = (
  hint: GitHubIssueJobHint,
  job: GitHubIssueJob | null,
  checkpoint: GitHubIssueJobCheckpoint | null = null,
): boolean =>
  hint.selection === null
    ? job === null
    : job !== null && hint.selection.repository === job.repository && hint.selection.issue_id === job.issueId &&
      hint.selection.node_id === job.nodeId && hint.selection.issue_number === job.number &&
      hint.selection.fingerprint === job.fingerprint &&
      JSON.stringify(hint.selection.checkpoint) ===
        JSON.stringify(
          checkpoint ? { branch: checkpoint.branch, sha: checkpoint.sha, base_sha: checkpoint.baseSha } : null,
        );

export const getCurrentGitHubIssueJob = async (
  source: GitHubIssueJobSource,
  repository: string,
  issueNumber: number,
): Promise<GitHubIssueJob | null> => {
  const issue = await source.getIssue(issueNumber);
  const relations = await source.getIssueRelations(issueNumber);
  const comments = await source.listIssueComments(issueNumber);
  return await createGitHubIssueJob(repository, issue, relations, comments);
};

export const selectNextGitHubIssueJobSelection = async (
  source: GitHubIssueJobSource,
  repository: string,
  ledgerMarkdown: string,
  recovery: SentinelRecoveryEligibilityContext,
  observedAt = new Date(),
  planning: GitHubIssueJobPlanningCallback | null = null,
): Promise<GitHubIssueJobSelection | null> => {
  if (!Number.isFinite(observedAt.getTime())) throw new Error("GitHub issue selection timestamp is invalid");
  // Validate the authoritative recovery context before the candidate loop so a
  // malformed snapshot fails closed even when every candidate is filtered out.
  const recoveryLedger = parseSentinelRecoveryLedger(recovery.ledger);
  if (recovery.repository.trim().length === 0 || !Number.isFinite(Date.parse(recovery.now))) {
    throw new Error("Sentinel recovery eligibility context is invalid");
  }
  const ledger = parseGitHubIssueJobLedger(ledgerMarkdown);
  const listed = await source.listOpenIssues();
  // Lightweight ordering by listed metadata; no author/editor, template,
  // estimate, comment, assignee, lock, relationship, or inspection-budget
  // preflight vetoes. The queue is exhausted only after every listed issue has
  // been inspected; transport pagination still bounds each single source read.
  const prioritized = [...listed].sort((left, right) =>
    (parseNumericQueuePriority(right.labels) ?? -1) - (parseNumericQueuePriority(left.labels) ?? -1) ||
    Date.parse(left.createdAt) - Date.parse(right.createdAt) || left.number - right.number
  );
  const queueEntries: GitHubIssueQueueEntry[] = [];
  for (const candidate of prioritized) {
    let current: GitHubIssue;
    let relations: GitHubIssueRelations | null = null;
    let job: GitHubIssueJob | null;
    try {
      current = await source.getIssue(candidate.number);
      if (current.id !== candidate.id || current.nodeId !== candidate.nodeId) {
        throw new Error(`GitHub issue ${candidate.number} identity changed during selection`);
      }
      const comments = await source.listIssueComments(candidate.number);
      relations = await source.getIssueRelations(candidate.number);
      job = await createGitHubIssueJob(repository, current, relations, comments);
    } catch {
      // A per-candidate source capture failure (fetch, source identity race,
      // comment/relationship read or projection) is isolated: only the safe
      // listed identity is reported, no partial source data is ever admitted,
      // and ordered selection continues to the next candidate. Global ledger
      // parsing/recovery validation and list failure stay fail-closed, and
      // planning/persistence errors are never caught here.
      queueEntries.push({
        number: candidate.number,
        node_id: candidate.nodeId,
        fingerprint: "",
        queue_priority: parseNumericQueuePriority(candidate.labels),
        selected: false,
        reason: "source_unavailable",
        next_action: "Resolve the unreadable issue source and re-inspect.",
      });
      continue;
    }
    if (job === null) {
      queueEntries.push({
        number: candidate.number,
        node_id: candidate.nodeId,
        fingerprint: "",
        queue_priority: parseNumericQueuePriority(candidate.labels),
        selected: false,
        reason: "source_unavailable",
        next_action: "Resolve the unreadable issue source and re-inspect.",
      });
      continue;
    }
    let selectionBlocked = false;
    let checkpoint: GitHubIssueJobCheckpoint | null = null;
    let blockingReason = "";
    const issueEntries = ledger.filter((entry) =>
      entry.issueId === job.issueId && entry.nodeId === job.nodeId && entry.number === job.number
    );
    const exactEntries = issueEntries.filter((entry) =>
      entry.fingerprint === job.fingerprint && entry.disposition !== "checkpoint_retained"
    );
    const activeRetryEntry = issueEntries.find((entry) => entry.disposition === "retry_pending");
    const terminalEntries = issueEntries.filter((entry) =>
      entry.disposition === "resolved" || entry.disposition === "manual_required"
    );
    // An active retry remains authoritative when it matches after inert-comment
    // normalization; an ordinary material discussion must never be normalized
    // away (materialDigest equality is mandatory).
    const entriesToInspect = exactEntries.length > 0
      ? exactEntries
      : activeRetryEntry
      ? [activeRetryEntry, ...terminalEntries]
      : terminalEntries;
    for (const entry of entriesToInspect) {
      let snapshotMatches = entry.fingerprint === job.fingerprint;
      if (!snapshotMatches) {
        // Inert-notice normalization is allowed only when the CURRENT capture
        // is proven inert-only (materialDigest null): then the historical
        // identity projection replays the old row's count/timestamp without
        // pretending to be a different captured comment list. Ordinary material
        // discussion is never normalized away, and differing inert counts do
        // not abort the queue.
        if (job.materialDigest === null) {
          const normalizedJob = await createGitHubIssueJobHistoricalProjection(
            repository,
            { ...current, comments: entry.comments, updatedAt: entry.sourceUpdatedAt },
            relations!,
          );
          snapshotMatches = normalizedJob?.fingerprint === entry.fingerprint;
        }
      }
      if (!snapshotMatches) continue;
      const retryReadyAt = Date.parse(entry.recordedAt) + GITHUB_ISSUE_JOB_RETRY_COOLDOWN_MS;
      if (entry.disposition !== "retry_pending" || observedAt.getTime() < retryReadyAt) {
        selectionBlocked = true;
        blockingReason = entry.disposition;
        break;
      }
      checkpoint = entry.checkpoint;
      // A due active retry that normalized to the current snapshot wins over
      // older terminal entries retained only for manual checkpoint recovery.
      break;
    }
    if (selectionBlocked) {
      queueEntries.push({
        number: job.number,
        node_id: job.nodeId,
        fingerprint: job.fingerprint,
        queue_priority: job.queuePriority,
        selected: false,
        reason: `issue_ledger_blocked:${blockingReason}`,
        next_action: "Wait for the recorded retry cooldown or terminal disposition.",
      });
      continue;
    }
    // The authoritative recovery state is consulted before the candidate is
    // returned: an unavailable first item must never starve a later eligible
    // item. The caller supplies the parsed snapshot (fetched once per
    // selection stage) and its exact continuation record for convergence.
    const eligibility = resolveSentinelRecoverySelection({
      ledger: recoveryLedger,
      repository,
      source_kind: "github_issue",
      source_id: String(job.issueId),
      source_revision: job.fingerprint,
      now: recovery.now,
      continuation_record: recovery.continuation_record ?? null,
    });
    if (!eligibility.eligibility.available) {
      queueEntries.push({
        number: job.number,
        node_id: job.nodeId,
        fingerprint: job.fingerprint,
        queue_priority: job.queuePriority,
        selected: false,
        reason: `recovery_unavailable:${eligibility.eligibility.reason}`,
        next_action: "Honor the authoritative terminal decision or wait for the due retry.",
      });
      continue;
    }
    if (planning !== null) {
      // Runtime planning runs only after both ledger and recovery eligibility:
      // a confirmed blocker is recorded and the ordered selection continues to
      // the next candidate instead of blocking the queue prefix.
      const planned = await planning(job, { checkpoint, recovery });
      if (!planned.ready) {
        queueEntries.push({
          number: job.number,
          node_id: job.nodeId,
          fingerprint: job.fingerprint,
          queue_priority: job.queuePriority,
          selected: false,
          reason: `planning_blocked:${planned.reason}`,
          next_action: planned.persisted
            ? "Persisted blocker holds this source; resolve it before review."
            : "Transient planning failure preserves the existing retry behavior.",
        });
        continue;
      }
      queueEntries.push({
        number: job.number,
        node_id: job.nodeId,
        fingerprint: job.fingerprint,
        queue_priority: job.queuePriority,
        selected: true,
        reason: "eligible_selected",
        next_action: "Prepare the frozen plan for this issue.",
      });
      return {
        job,
        checkpoint,
        triage: planned.triage,
        queue: { queue_exhausted: false, entries: queueEntries },
      };
    }
    queueEntries.push({
      number: job.number,
      node_id: job.nodeId,
      fingerprint: job.fingerprint,
      queue_priority: job.queuePriority,
      selected: true,
      reason: "eligible_selected",
      next_action: "Prepare the frozen plan for this issue.",
    });
    return {
      job,
      checkpoint,
      triage: null,
      queue: { queue_exhausted: false, entries: queueEntries },
    };
  }
  // Every listed issue was inspected without a selection: the queue itself is
  // exhausted, never a budget/prefix artifact.
  return {
    job: null,
    checkpoint: null,
    triage: null,
    queue: { queue_exhausted: true, entries: queueEntries },
  };
};

export const selectNextGitHubIssueJob = async (
  source: GitHubIssueJobSource,
  repository: string,
  ledgerMarkdown: string,
  recovery: SentinelRecoveryEligibilityContext,
  observedAt = new Date(),
): Promise<GitHubIssueJob | null> =>
  (await selectNextGitHubIssueJobSelection(source, repository, ledgerMarkdown, recovery, observedAt))?.job ?? null;

export const githubIssueJobTriageReport = (
  job: GitHubIssueJob,
  interval: SentinelInterval,
): TriageReport => {
  const scopeName = job.intake === "owner_backlog" ? "extracted source-file" : "declared Files";
  return {
    schema_version: 1,
    interval,
    findings: [{
      id: `github-issue:${job.number}:${job.fingerprint}`,
      fingerprint: job.fingerprint,
      severity: job.priority,
      title: job.title,
      affected_surface: job.files.join(", "),
      allowed_paths: [...job.files],
      shared_paths: [],
      depends_on: [],
      evidence: [{
        source: "github_issue",
        reference: job.htmlUrl,
        detail: job.body,
      }],
      proposed_correction:
        `Implement GitHub issue #${job.number} within its ${scopeName} scope and satisfy every acceptance item.`,
      validation_requirements: [
        ...job.acceptance,
        `Change only ${scopeName} issue paths: ${job.files.join(", ")}`,
        "Run repository formatting, lint, build, and affected tests",
      ],
      actionable: true,
    }],
    no_findings_reason: null,
  };
};

/**
 * Planned-scope path matching: exact files match exactly; explicit directory
 * entries (trailing slash) match descendants at a path-component boundary.
 * `src/foo/` never authorizes `src/foobar.ts`; protected descendants are
 * enforced independently by the protected-path matcher.
 */
export const issuePathWithinPlannedScope = (path: string, scope: readonly string[]): boolean =>
  // Shared matrix representation: explicit trailing-slash directories match
  // descendants at a component boundary; non-slash entries are exact files.
  scope.some((entry) => matrixAllowedPathCovers(path, entry));

export const blockingIssueReviewFindings = (
  report: NativeReviewReport,
  files: readonly string[],
  plannedScope?: readonly string[],
): NativeReviewFinding[] => {
  if (report.parse_status === "unparseable") throw new Error("Native Codex review output was not parseable");
  const scope = plannedScope ?? files;
  return report.findings.filter((finding) => {
    if (finding.severity === "P0" || finding.severity === "P1") return true;
    const locationPath = finding.location.match(FILE_LOCATION_PATTERN)?.[1];
    return locationPath !== undefined && issuePathWithinPlannedScope(locationPath, scope);
  });
};

export const issueReviewBacklogFindings = (
  report: NativeReviewReport,
  files: readonly string[],
  plannedScope?: readonly string[],
): NativeReviewFinding[] => {
  const blockers = new Set(blockingIssueReviewFindings(report, files, plannedScope));
  return report.findings.filter((finding) =>
    (finding.severity === "P2" || finding.severity === "P3") && !blockers.has(finding)
  );
};

export const evaluateGitHubIssueJobImplementation = (
  job: GitHubIssueJob,
  status: "implemented" | "already_fixed" | "not_actionable" | "blocked",
  actualChangedPaths: readonly string[],
  reportedChangedPaths: readonly string[],
  plannedScope: readonly string[] | null = null,
): Readonly<{
  disposition: Exclude<GitHubIssueJobDisposition, "checkpoint_retained">;
  continueToRuntimeValidation: boolean;
}> => {
  const actual = sortedUnique(actualChangedPaths);
  const reported = sortedUnique(reportedChangedPaths);
  if (
    !actual || !reported || actual.length !== reported.length ||
    !actual.every((path, index) => path === reported[index])
  ) {
    throw new SentinelChangedFilesMismatchError(
      "GitHub issue implementation report changed_files does not match the candidate diff",
      actual ?? [...actualChangedPaths],
      reported ?? [...reportedChangedPaths],
      "github_issue",
    );
  }
  // The frozen execution scope is the planned scope (exact files or explicit
  // directories); source hints are never execution authority for fresh work.
  const scope = plannedScope ?? job.files;
  if (actual.some((path) => isSentinelProtectedImplementationPath(path))) {
    throw new Error("GitHub issue implementation changed a protected path");
  }
  if (actual.some((path) => !issuePathWithinPlannedScope(path, scope))) {
    throw new Error("GitHub issue implementation changed a path outside the frozen planned scope");
  }
  if (status === "implemented" && actual.length > 0) {
    return { disposition: "resolved", continueToRuntimeValidation: true };
  }
  if (actual.length > 0) {
    throw new Error(`GitHub issue implementation status ${status} cannot retain candidate code changes`);
  }
  return { disposition: "manual_required", continueToRuntimeValidation: false };
};

export const requireResolvedGitHubIssueJobImplementation = (
  job: GitHubIssueJob,
  status: "implemented" | "already_fixed" | "not_actionable" | "blocked",
  actualChangedPaths: readonly string[],
  reportedChangedPaths: readonly string[],
  plannedScope: readonly string[] | null = null,
): void => {
  const decision = evaluateGitHubIssueJobImplementation(
    job,
    status,
    actualChangedPaths,
    reportedChangedPaths,
    plannedScope,
  );
  if (decision.disposition !== "resolved" || !decision.continueToRuntimeValidation) {
    throw new Error("The selected GitHub issue repair does not retain a matching aggregate candidate code diff");
  }
};

const cleanCell = (value: string, maximum = 512): string =>
  value.trim().replaceAll("\r", " ").replaceAll("\n", " ").replace(/\s+/gu, " ").slice(0, maximum)
    .replace(/[&|<>`]|[^\x20-\x7e]/gu, (character) => {
      if (character === "&") return "&amp;";
      if (character === "|") return "&#124;";
      if (character === "<") return "&lt;";
      if (character === ">") return "&gt;";
      if (character === "`") return "&#96;";
      return `&#x${character.codePointAt(0)!.toString(16)};`;
    });

const decodeCell = (value: string): string =>
  value.replace(/&#x([0-9a-f]+);/giu, (_match, encoded: string) => String.fromCodePoint(Number.parseInt(encoded, 16)))
    .replace(/&#(\d+);/gu, (_match, encoded: string) => String.fromCodePoint(Number.parseInt(encoded, 10)))
    .replaceAll("&lt;", "<").replaceAll("&gt;", ">").replaceAll("&amp;", "&");

const sortedLedgerEntries = (entries: readonly GitHubIssueJobLedgerEntry[]): GitHubIssueJobLedgerEntry[] =>
  [...entries].sort((left, right) =>
    left.number - right.number || Date.parse(left.recordedAt) - Date.parse(right.recordedAt)
  );

export const renderGitHubIssueJobLedger = (entries: readonly GitHubIssueJobLedgerEntry[]): string => {
  if (entries.length > MAX_LEDGER_ENTRIES) throw new Error("Sentinel issue-job ledger exceeds its entry limit");
  const rows = sortedLedgerEntries(entries).map((entry) => [
    `#${entry.number}`,
    String(entry.issueId),
    `\`${entry.nodeId}\``,
    `\`${entry.fingerprint}\``,
    `\`${entry.bodySha256}\``,
    String(entry.comments),
    entry.sourceUpdatedAt,
    entry.recordedAt,
    `\`${entry.baseSha}\``,
    entry.checkpoint ? `\`${entry.checkpoint.branch}\`` : "",
    entry.checkpoint ? `\`${entry.checkpoint.sha}\`` : "",
    cleanCell(entry.title),
    entry.disposition,
  ]);
  const widths = LEDGER_HEADERS.map((header, index) =>
    Math.max(header.length, ...rows.map((row) => row[index]!.length))
  );
  const renderRow = (cells: readonly string[]): string =>
    `| ${cells.map((cell, index) => cell.padEnd(widths[index]!)).join(" | ")} |`;
  const table = [
    renderRow(LEDGER_HEADERS),
    renderRow(widths.map((width) => "-".repeat(width))),
    ...rows.map(renderRow),
  ];
  if (table.some((line) => line.length > MAX_LEDGER_LINE_LENGTH)) {
    throw new Error("Sentinel issue-job ledger row exceeds its length limit");
  }
  const markdown = [
    "# Sentinel Issue Job Ledger",
    "",
    "Sentinel results for immutable GitHub issue snapshots are tracked here. A retry-pending snapshot waits six hours before",
    "it is eligible again so later issues can advance. A retry checkpoint names an immutable remote candidate that Sentinel",
    "may resume when the snapshot matches after accepted inert UbiquityOS notices are normalized. A manual-required",
    "checkpoint retains an unsafe reviewed candidate for a person and never starts a retry, pull request, preview, or",
    "deployment. A superseded checkpoint remains auditable as nonblocking checkpoint_retained evidence. Terminal snapshots",
    "are delivered through exactly one pull request that links the issue as evidence. After a verified production keep,",
    "Sentinel merges the delivery pull request and closes the unchanged issue with supporting evidence; a pull request",
    "already carried by the development push is accepted after a containment check. Manual-required, failed, and rolled-back",
    "results remain open.",
    "",
    ...table,
    "",
  ].join("\n");
  if (textEncoder.encode(markdown).byteLength > MAX_LEDGER_BYTES) {
    throw new Error("Sentinel issue-job ledger exceeds its byte limit");
  }
  return markdown;
};

export const parseGitHubIssueJobLedger = (markdown: string): GitHubIssueJobLedgerEntry[] => {
  if (textEncoder.encode(markdown).byteLength > MAX_LEDGER_BYTES) {
    throw new Error("Sentinel issue-job ledger exceeds its byte limit");
  }
  const entries: GitHubIssueJobLedgerEntry[] = [];
  const identities = new Set<string>();
  for (const line of markdown.replaceAll("\r\n", "\n").split("\n")) {
    const row = line.trim();
    if (!row.startsWith("|")) continue;
    if (!row.endsWith("|") || row.length > MAX_LEDGER_LINE_LENGTH) {
      throw new Error("Sentinel issue-job ledger contains an invalid row");
    }
    const cells = row.slice(1, -1).split("|").map((cell) => cell.trim());
    if (cells.length === LEDGER_HEADERS.length && cells.every((cell, index) => cell === LEDGER_HEADERS[index])) {
      continue;
    }
    if (cells.length === LEDGER_HEADERS.length && cells.every((cell) => /^-+$/u.test(cell))) continue;
    if (cells.length !== LEDGER_HEADERS.length) throw new Error("Sentinel issue-job ledger row has the wrong shape");
    const number = /^#[1-9][0-9]*$/u.test(cells[0]!) ? Number(cells[0]!.slice(1)) : Number.NaN;
    const issueId = /^[1-9][0-9]*$/u.test(cells[1]!) ? Number(cells[1]) : Number.NaN;
    const nodeId = /^`[^`]+`$/u.test(cells[2]!) ? cells[2]!.slice(1, -1) : "";
    const fingerprint = /^`[0-9a-f]{64}`$/u.test(cells[3]!) ? cells[3]!.slice(1, -1) : "";
    const bodySha256 = /^`[0-9a-f]{64}`$/u.test(cells[4]!) ? cells[4]!.slice(1, -1) : "";
    const comments = /^(?:0|[1-9][0-9]*)$/u.test(cells[5]!) ? Number(cells[5]) : Number.NaN;
    const sourceUpdatedAt = cells[6]!;
    const recordedAt = cells[7]!;
    const baseSha = /^`[0-9a-f]{40}`$/u.test(cells[8]!) ? cells[8]!.slice(1, -1) : "";
    const checkpointBranch = /^`[^`]+`$/u.test(cells[9]!) ? cells[9]!.slice(1, -1) : "";
    const checkpointSha = /^`[0-9a-f]{40}`$/u.test(cells[10]!) ? cells[10]!.slice(1, -1) : "";
    const checkpoint = checkpointBranch || checkpointSha
      ? { branch: checkpointBranch, sha: checkpointSha, baseSha }
      : null;
    const title = decodeCell(cells[11]!);
    const disposition = cells[12] as GitHubIssueJobDisposition;
    const identity = `${issueId}:${number}:${fingerprint}`;
    if (
      !Number.isSafeInteger(number) || number <= 0 || !Number.isSafeInteger(issueId) || issueId <= 0 ||
      !NODE_ID.test(nodeId) || !SHA256.test(fingerprint) || !SHA256.test(bodySha256) ||
      !Number.isSafeInteger(comments) || comments < 0 ||
      !validTimestamp(sourceUpdatedAt) || !validTimestamp(recordedAt) || !FULL_SHA.test(baseSha) ||
      ((checkpointBranch === "") !== (checkpointSha === "")) ||
      (checkpoint !== null &&
        (!CHECKPOINT_BRANCH.test(checkpoint.branch) || !FULL_SHA.test(checkpoint.sha) || checkpoint.sha === baseSha)) ||
      title.trim().length === 0 || title.length > 512 ||
      (disposition !== "retry_pending" && disposition !== "checkpoint_retained" && disposition !== "resolved" &&
        disposition !== "manual_required") ||
      (disposition === "checkpoint_retained" && checkpoint === null) ||
      (disposition === "resolved" && checkpoint !== null) ||
      identities.has(identity)
    ) throw new Error("Sentinel issue-job ledger row is invalid");
    identities.add(identity);
    entries.push({
      issueId,
      nodeId,
      number,
      fingerprint,
      bodySha256,
      comments,
      sourceUpdatedAt,
      recordedAt,
      baseSha,
      checkpoint,
      title,
      disposition,
    });
  }
  const parsed = sortedLedgerEntries(entries);
  const activeRetryIssues = new Set<string>();
  for (const entry of parsed) {
    if (entry.disposition !== "retry_pending") continue;
    const issueIdentity = `${entry.issueId}:${entry.nodeId}:${entry.number}`;
    if (activeRetryIssues.has(issueIdentity)) {
      throw new Error("Sentinel issue-job ledger contains multiple active retries for one issue");
    }
    activeRetryIssues.add(issueIdentity);
  }
  if (markdown !== renderGitHubIssueJobLedger(parsed)) {
    throw new Error("Sentinel issue-job ledger is not in its canonical complete form");
  }
  return parsed;
};

export const applyGitHubIssueJobDisposition = (
  markdown: string,
  job: GitHubIssueJob,
  baseSha: string,
  observedAt: Date,
  disposition: GitHubIssueJobDisposition,
  checkpoint: GitHubIssueJobCheckpoint | null = null,
): string => {
  if (
    !FULL_SHA.test(baseSha) || !Number.isFinite(observedAt.getTime()) ||
    (checkpoint !== null &&
      (!CHECKPOINT_BRANCH.test(checkpoint.branch) || !FULL_SHA.test(checkpoint.sha) ||
        checkpoint.baseSha !== baseSha || checkpoint.sha === baseSha)) ||
    (disposition === "resolved" && checkpoint !== null)
  ) {
    throw new Error("Sentinel issue-job disposition metadata is invalid");
  }
  const entries = parseGitHubIssueJobLedger(markdown);
  const nextEntry: GitHubIssueJobLedgerEntry = {
    issueId: job.issueId,
    nodeId: job.nodeId,
    number: job.number,
    fingerprint: job.fingerprint,
    bodySha256: job.bodySha256,
    comments: job.comments,
    sourceUpdatedAt: job.updatedAt,
    recordedAt: observedAt.toISOString(),
    baseSha,
    checkpoint,
    title: job.title,
    disposition,
  };
  const exactExisting = entries.find((entry) =>
    entry.issueId === job.issueId && entry.nodeId === job.nodeId && entry.number === job.number &&
    entry.fingerprint === job.fingerprint
  );
  if (
    exactExisting && exactExisting.disposition !== "retry_pending" &&
    exactExisting.disposition !== "checkpoint_retained"
  ) {
    throw new Error("The selected GitHub issue snapshot already has a terminal Sentinel disposition");
  }
  const priorPending = entries.filter((entry) =>
    entry.issueId === job.issueId && entry.nodeId === job.nodeId && entry.number === job.number &&
    entry.disposition === "retry_pending"
  );
  for (const existing of priorPending) {
    if (Date.parse(existing.recordedAt) > observedAt.getTime()) {
      throw new Error("The selected GitHub issue retry timestamp cannot move backwards");
    }
  }
  const retainedPriorCheckpoints = priorPending
    .filter((entry) => entry.fingerprint !== job.fingerprint && entry.checkpoint !== null)
    .map((entry) => ({ ...entry, disposition: "checkpoint_retained" as const }));
  const replacedRetainedCheckpoint = exactExisting?.disposition === "checkpoint_retained" ? exactExisting : null;
  return renderGitHubIssueJobLedger([
    ...entries.filter((entry) => !priorPending.includes(entry) && entry !== replacedRetainedCheckpoint),
    ...retainedPriorCheckpoints,
    nextEntry,
  ]);
};

export const issueJobFindingId = (job: GitHubIssueJob): string => `github-issue:${job.number}:${job.fingerprint}`;

export const issueJobProtectedControlPaths = (): readonly string[] => [
  SENTINEL_POLICY.paths.reviewBacklog,
  SENTINEL_POLICY.paths.issueJobLedger,
];
