import type { GitHubIssue, GitHubIssueRelations, GitHubRepositoryPermission } from "./github.ts";
import { isSentinelProtectedImplementationPath, SENTINEL_POLICY } from "./policy.ts";
import type {
  NativeReviewFinding,
  NativeReviewReport,
  SentinelInterval,
  TriageReport,
  TriageSeverity,
} from "./types.ts";

export interface GitHubIssueJobSource {
  listOpenIssues(): Promise<readonly GitHubIssue[]>;
  getIssue(issueNumber: number): Promise<GitHubIssue>;
  getIssueRelations(issueNumber: number): Promise<GitHubIssueRelations>;
  getRepositoryPermission(username: string): Promise<GitHubRepositoryPermission>;
}

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
  priority: "P2" | "P3";
  priorityLabel: "Priority: 2 (Medium)" | "Priority: 3 (High)";
  timeLabel: "Time: <15 Minutes" | "Time: <1 Hour" | "Time: <2 Hours";
  labels: readonly string[];
  files: readonly string[];
  acceptance: readonly string[];
  authorLogin: string;
  authorAssociation: string;
  comments: 0;
  createdAt: string;
  updatedAt: string;
  relations: GitHubIssueRelations;
}>;

export type GitHubIssueJobDisposition = "resolved" | "manual_required";

export type GitHubIssueJobHint = Readonly<{
  schema_version: 1;
  selection:
    | Readonly<{
      repository: string;
      issue_id: number;
      node_id: string;
      issue_number: number;
      fingerprint: string;
    }>
    | null;
}>;

export type GitHubIssueJobLedgerEntry = Readonly<{
  issueId: number;
  nodeId: string;
  number: number;
  fingerprint: string;
  bodySha256: string;
  sourceUpdatedAt: string;
  recordedAt: string;
  baseSha: string;
  title: string;
  disposition: GitHubIssueJobDisposition;
}>;

const PRIORITY_LABELS = new Map<GitHubIssueJob["priorityLabel"], GitHubIssueJob["priority"]>([
  ["Priority: 2 (Medium)", "P3"],
  ["Priority: 3 (High)", "P2"],
]);
const ISSUE_QUEUE_RANK = new Map<GitHubIssueJob["priorityLabel"], number>([
  ["Priority: 3 (High)", 0],
  ["Priority: 2 (Medium)", 1],
]);
const TIME_LABELS = new Set<GitHubIssueJob["timeLabel"]>([
  "Time: <15 Minutes",
  "Time: <1 Hour",
  "Time: <2 Hours",
]);
const FILE_LOCATION_PATTERN = /^([A-Za-z0-9_.@/+\-]+)(?::\d+(?:-\d+)?(?::\d+)?)?$/u;
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const NODE_ID = /^[A-Za-z0-9_=-]{4,160}$/u;
const MAX_ISSUE_BODY_BYTES = 32 * 1_024;
const MAX_ISSUE_FILES = 32;
const MAX_ACCEPTANCE_ITEMS = 32;
const MAX_LEDGER_BYTES = 256 * 1_024;
const MAX_LEDGER_ENTRIES = 512;
const MAX_LEDGER_LINE_LENGTH = 4_096;
const MAX_ISSUE_JOB_HINT_BYTES = 1_024;
export const MAX_ISSUE_JOB_CANDIDATES = 32;
export const GITHUB_ISSUE_JOB_HINT_FILENAME = "sentinel-github-issue-job-hint.json";
const LEDGER_HEADERS = Object.freeze([
  "Issue",
  "REST ID",
  "Node ID",
  "Fingerprint",
  "Body SHA-256",
  "Source updated",
  "Recorded",
  "Base SHA",
  "Title",
  "Disposition",
]);

const textEncoder = new TextEncoder();

const record = (value: unknown): Record<string, unknown> | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : null;

const hasExactKeys = (value: Record<string, unknown>, keys: readonly string[]): boolean => {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  return actual.length === expected.length && actual.every((key, index) => key === expected[index]);
};

const sha256 = async (value: string): Promise<string> => {
  const digest = await crypto.subtle.digest("SHA-256", textEncoder.encode(value));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

const validTimestamp = (value: string): boolean =>
  /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/u.test(value) && Number.isFinite(Date.parse(value));

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
  if (!canonicalFiles) return null;
  return { acceptance, files: canonicalFiles };
};

const issueJobLabels = (
  labels: readonly string[],
):
  | Readonly<{
    labels: readonly string[];
    priority: GitHubIssueJob["priority"];
    priorityLabel: GitHubIssueJob["priorityLabel"];
    timeLabel: GitHubIssueJob["timeLabel"];
  }>
  | null => {
  const canonicalLabels = sortedUnique(labels);
  if (!canonicalLabels) return null;
  const priorityLabels = canonicalLabels.filter((label) => label.startsWith("Priority: "));
  const timeLabels = canonicalLabels.filter((label) => label.startsWith("Time: "));
  if (priorityLabels.length !== 1 || timeLabels.length !== 1) return null;
  const priorityLabel = priorityLabels[0] as GitHubIssueJob["priorityLabel"];
  const timeLabel = timeLabels[0] as GitHubIssueJob["timeLabel"];
  const priority = PRIORITY_LABELS.get(priorityLabel);
  if (!priority || !TIME_LABELS.has(timeLabel)) return null;
  return { labels: canonicalLabels, priority, priorityLabel, timeLabel };
};

const issueAuthorityLogins = (
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
): readonly string[] | null => {
  const bodyEdit = relations.latestBodyEdit;
  const titleEdit = relations.latestTitleEdit;
  for (const edit of [bodyEdit, titleEdit]) {
    if (
      edit !== null &&
      (edit.editorLogin.trim() !== edit.editorLogin || edit.editorLogin.length === 0 ||
        !validTimestamp(edit.editedAt) || Date.parse(edit.editedAt) < Date.parse(issue.createdAt) ||
        Date.parse(edit.editedAt) > Date.parse(issue.updatedAt))
    ) return null;
  }
  if (titleEdit !== null && titleEdit.title !== issue.title) return null;
  return [
    ...new Set([
      issue.authorLogin,
      ...(bodyEdit === null ? [] : [bodyEdit.editorLogin]),
      ...(titleEdit === null ? [] : [titleEdit.editorLogin]),
    ]),
  ].sort();
};

const baseIssueEligible = (issue: GitHubIssue, authorityPermission: GitHubRepositoryPermission): boolean =>
  issue.state === "open" && !issue.isPullRequest && !issue.locked && issue.assignees.length === 0 &&
  issue.comments === 0 && issue.title.length <= 256 &&
  (authorityPermission === "write" || authorityPermission === "admin");

export const createGitHubIssueJob = async (
  repository: string,
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
  authorityPermission: GitHubRepositoryPermission,
): Promise<GitHubIssueJob | null> => {
  validatedRepository(repository);
  if (
    !baseIssueEligible(issue, authorityPermission) || issueAuthorityLogins(issue, relations) === null ||
    relations.parentIssueNumber !== null ||
    relations.subIssueCount !== 0 || relations.blockedByCount !== 0 ||
    relations.blockingCount !== 0 || !validTimestamp(issue.createdAt) || !validTimestamp(issue.updatedAt) ||
    Date.parse(issue.updatedAt) < Date.parse(issue.createdAt) || !validIssueUrl(issue.htmlUrl, repository, issue.number)
  ) return null;
  const parsedBody = parseGitHubIssueJobBody(issue.body);
  const parsedLabels = issueJobLabels(issue.labels);
  if (!parsedBody || !parsedLabels) return null;
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
    labels: parsedLabels.labels,
    assignees: issue.assignees,
    locked: issue.locked,
    comments: issue.comments,
    created_at: issue.createdAt,
    updated_at: issue.updatedAt,
    relations,
    files: parsedBody.files,
    acceptance: parsedBody.acceptance,
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
    priority: parsedLabels.priority,
    priorityLabel: parsedLabels.priorityLabel,
    timeLabel: parsedLabels.timeLabel,
    labels: parsedLabels.labels,
    files: parsedBody.files,
    acceptance: parsedBody.acceptance,
    authorLogin: issue.authorLogin,
    authorAssociation: issue.authorAssociation,
    comments: 0,
    createdAt: issue.createdAt,
    updatedAt: issue.updatedAt,
    relations,
  };
};

const issueJobOrder = (left: GitHubIssueJob, right: GitHubIssueJob): number =>
  ISSUE_QUEUE_RANK.get(left.priorityLabel)! - ISSUE_QUEUE_RANK.get(right.priorityLabel)! ||
  Date.parse(left.createdAt) - Date.parse(right.createdAt) || left.number - right.number;

export const githubIssueJobsMatch = (expected: GitHubIssueJob, actual: GitHubIssueJob | null): boolean =>
  actual !== null && expected.repository === actual.repository && expected.issueId === actual.issueId &&
  expected.nodeId === actual.nodeId && expected.number === actual.number && expected.fingerprint === actual.fingerprint;

const githubIssueJobSourceSnapshotsMatch = (expected: GitHubIssueJob, actual: GitHubIssueJob): boolean =>
  expected.repository === actual.repository && expected.issueId === actual.issueId &&
  expected.nodeId === actual.nodeId &&
  expected.number === actual.number && expected.htmlUrl === actual.htmlUrl && expected.title === actual.title &&
  expected.bodySha256 === actual.bodySha256 && expected.priority === actual.priority &&
  expected.priorityLabel === actual.priorityLabel && expected.timeLabel === actual.timeLabel &&
  expected.authorLogin === actual.authorLogin && expected.authorAssociation === actual.authorAssociation &&
  expected.createdAt === actual.createdAt && expected.updatedAt === actual.updatedAt &&
  JSON.stringify(expected.labels) === JSON.stringify(actual.labels) &&
  JSON.stringify(expected.files) === JSON.stringify(actual.files) &&
  JSON.stringify(expected.acceptance) === JSON.stringify(actual.acceptance);

const issueAuthorityPermission = async (
  source: GitHubIssueJobSource,
  issue: GitHubIssue,
  relations: GitHubIssueRelations,
): Promise<GitHubRepositoryPermission> => {
  const logins = issueAuthorityLogins(issue, relations);
  if (logins === null) return "none";
  const permissions = await Promise.all(logins.map((login) => source.getRepositoryPermission(login)));
  return permissions.every((permission) => permission === "write" || permission === "admin") ? "write" : "none";
};

export const renderGitHubIssueJobHint = (job: GitHubIssueJob | null): string =>
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
    !hasExactKeys(selection, ["repository", "issue_id", "node_id", "issue_number", "fingerprint"]) ||
    typeof selection.repository !== "string" || validatedRepository(selection.repository) !== selection.repository ||
    !Number.isSafeInteger(selection.issue_id) || (selection.issue_id as number) <= 0 ||
    typeof selection.node_id !== "string" || !NODE_ID.test(selection.node_id) ||
    !Number.isSafeInteger(selection.issue_number) || (selection.issue_number as number) <= 0 ||
    typeof selection.fingerprint !== "string" || !SHA256.test(selection.fingerprint)
  ) throw new Error("Sentinel GitHub issue-job hint has an invalid selection");
  return {
    schema_version: 1,
    selection: {
      repository: selection.repository,
      issue_id: selection.issue_id as number,
      node_id: selection.node_id,
      issue_number: selection.issue_number as number,
      fingerprint: selection.fingerprint,
    },
  };
};

export const githubIssueJobMatchesHint = (hint: GitHubIssueJobHint, job: GitHubIssueJob | null): boolean =>
  hint.selection === null
    ? job === null
    : job !== null && hint.selection.repository === job.repository && hint.selection.issue_id === job.issueId &&
      hint.selection.node_id === job.nodeId && hint.selection.issue_number === job.number &&
      hint.selection.fingerprint === job.fingerprint;

export const getCurrentGitHubIssueJob = async (
  source: GitHubIssueJobSource,
  repository: string,
  issueNumber: number,
): Promise<GitHubIssueJob | null> => {
  const issue = await source.getIssue(issueNumber);
  const relations = await source.getIssueRelations(issueNumber);
  const authorityPermission = await issueAuthorityPermission(source, issue, relations);
  return await createGitHubIssueJob(repository, issue, relations, authorityPermission);
};

export const selectNextGitHubIssueJob = async (
  source: GitHubIssueJobSource,
  repository: string,
  ledgerMarkdown: string,
): Promise<GitHubIssueJob | null> => {
  const ledger = parseGitHubIssueJobLedger(ledgerMarkdown);
  const listed = await source.listOpenIssues();
  const candidates: GitHubIssueJob[] = [];
  for (const candidate of listed) {
    const job = await createGitHubIssueJob(repository, candidate, {
      parentIssueNumber: null,
      subIssueCount: 0,
      blockedByCount: 0,
      blockingCount: 0,
      latestBodyEdit: null,
      latestTitleEdit: null,
    }, "write");
    if (!job) continue;
    candidates.push(job);
  }
  candidates.sort(issueJobOrder);
  for (const [index, candidate] of candidates.entries()) {
    if (index >= MAX_ISSUE_JOB_CANDIDATES) {
      throw new Error("GitHub issue selection exceeded the Sentinel candidate inspection limit");
    }
    const current = await source.getIssue(candidate.number);
    if (current.id !== candidate.issueId || current.nodeId !== candidate.nodeId) {
      throw new Error(`GitHub issue ${candidate.number} identity changed during selection`);
    }
    const relations = await source.getIssueRelations(candidate.number);
    const authorityPermission = await issueAuthorityPermission(source, current, relations);
    const job = await createGitHubIssueJob(repository, current, relations, authorityPermission);
    if (!job) continue;
    if (!githubIssueJobSourceSnapshotsMatch(candidate, job)) {
      throw new Error(`GitHub issue ${candidate.number} snapshot changed during selection`);
    }
    if (
      ledger.some((entry) =>
        entry.issueId === job.issueId && entry.nodeId === job.nodeId && entry.number === job.number &&
        entry.fingerprint === job.fingerprint
      )
    ) continue;
    return job;
  }
  return null;
};

export const githubIssueJobTriageReport = (
  job: GitHubIssueJob,
  interval: SentinelInterval,
): TriageReport => ({
  schema_version: 1,
  interval,
  findings: [{
    id: `github-issue:${job.number}:${job.fingerprint}`,
    fingerprint: job.fingerprint,
    severity: job.priority,
    title: job.title,
    affected_surface: job.files.join(", "),
    evidence: [{
      source: "github_issue",
      reference: job.htmlUrl,
      detail: job.body,
    }],
    proposed_correction:
      `Implement GitHub issue #${job.number} within its declared Files scope and satisfy every Acceptance item.`,
    validation_requirements: [
      ...job.acceptance,
      `Change only declared issue files: ${job.files.join(", ")}`,
      "Run repository formatting, lint, build, and affected tests",
    ],
    actionable: true,
  }],
  no_findings_reason: null,
});

export const blockingIssueReviewFindings = (
  report: NativeReviewReport,
  files: readonly string[],
): NativeReviewFinding[] => {
  if (report.parse_status === "unparseable") throw new Error("Native Codex review output was not parseable");
  const fileSet = new Set(files);
  return report.findings.filter((finding) => {
    if (finding.severity === "P0" || finding.severity === "P1") return true;
    const locationPath = finding.location.match(FILE_LOCATION_PATTERN)?.[1];
    return locationPath !== undefined && fileSet.has(locationPath);
  });
};

export const issueReviewBacklogFindings = (
  report: NativeReviewReport,
  files: readonly string[],
): NativeReviewFinding[] => {
  const blockers = new Set(blockingIssueReviewFindings(report, files));
  return report.findings.filter((finding) =>
    (finding.severity === "P2" || finding.severity === "P3") && !blockers.has(finding)
  );
};

export const evaluateGitHubIssueJobImplementation = (
  job: GitHubIssueJob,
  status: "implemented" | "already_fixed" | "not_actionable" | "blocked",
  actualChangedPaths: readonly string[],
  reportedChangedPaths: readonly string[],
): Readonly<{ disposition: GitHubIssueJobDisposition; continueToRuntimeValidation: boolean }> => {
  const actual = sortedUnique(actualChangedPaths);
  const reported = sortedUnique(reportedChangedPaths);
  if (
    !actual || !reported || actual.length !== reported.length ||
    !actual.every((path, index) => path === reported[index])
  ) {
    throw new Error("GitHub issue implementation report changed_files does not match the candidate diff");
  }
  if (actual.some((path) => !job.files.includes(path))) {
    throw new Error("GitHub issue implementation changed a path outside the declared Files scope");
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
): void => {
  const decision = evaluateGitHubIssueJobImplementation(job, status, actualChangedPaths, reportedChangedPaths);
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
    entry.sourceUpdatedAt,
    entry.recordedAt,
    `\`${entry.baseSha}\``,
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
    "Terminal Sentinel results for immutable GitHub issue snapshots are tracked here. Editing an open issue creates a new",
    "snapshot that can become eligible again. The Sentinel does not assign, label, comment on, or close issues.",
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
    const sourceUpdatedAt = cells[5]!;
    const recordedAt = cells[6]!;
    const baseSha = /^`[0-9a-f]{40}`$/u.test(cells[7]!) ? cells[7]!.slice(1, -1) : "";
    const title = decodeCell(cells[8]!);
    const disposition = cells[9] as GitHubIssueJobDisposition;
    const identity = `${issueId}:${number}:${fingerprint}`;
    if (
      !Number.isSafeInteger(number) || number <= 0 || !Number.isSafeInteger(issueId) || issueId <= 0 ||
      !NODE_ID.test(nodeId) || !SHA256.test(fingerprint) || !SHA256.test(bodySha256) ||
      !validTimestamp(sourceUpdatedAt) || !validTimestamp(recordedAt) || !FULL_SHA.test(baseSha) ||
      title.trim().length === 0 || title.length > 512 ||
      (disposition !== "resolved" && disposition !== "manual_required") || identities.has(identity)
    ) throw new Error("Sentinel issue-job ledger row is invalid");
    identities.add(identity);
    entries.push({
      issueId,
      nodeId,
      number,
      fingerprint,
      bodySha256,
      sourceUpdatedAt,
      recordedAt,
      baseSha,
      title,
      disposition,
    });
  }
  const parsed = sortedLedgerEntries(entries);
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
): string => {
  if (!FULL_SHA.test(baseSha) || !Number.isFinite(observedAt.getTime())) {
    throw new Error("Sentinel issue-job disposition metadata is invalid");
  }
  const entries = parseGitHubIssueJobLedger(markdown);
  if (
    entries.some((entry) =>
      entry.issueId === job.issueId && entry.nodeId === job.nodeId && entry.number === job.number &&
      entry.fingerprint === job.fingerprint
    )
  ) throw new Error("The selected GitHub issue snapshot already has a terminal Sentinel disposition");
  entries.push({
    issueId: job.issueId,
    nodeId: job.nodeId,
    number: job.number,
    fingerprint: job.fingerprint,
    bodySha256: job.bodySha256,
    sourceUpdatedAt: job.updatedAt,
    recordedAt: observedAt.toISOString(),
    baseSha,
    title: job.title,
    disposition,
  });
  return renderGitHubIssueJobLedger(entries);
};

export const issueJobFindingId = (job: GitHubIssueJob): string => `github-issue:${job.number}:${job.fingerprint}`;

export const issueJobProtectedControlPaths = (): readonly string[] => [
  SENTINEL_POLICY.paths.reviewBacklog,
  SENTINEL_POLICY.paths.issueJobLedger,
];

export const issuePrioritySeverity = (label: string): TriageSeverity | null =>
  PRIORITY_LABELS.get(label as GitHubIssueJob["priorityLabel"]) ?? null;
