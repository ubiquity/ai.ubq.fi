import { SENTINEL_POLICY } from "./policy.ts";
import { type NativeReviewFinding, type NativeReviewReport, type TriageSeverity } from "./types.ts";
import { mergeReviewBacklog, parseNativeReview, parseStructuredNativeReview } from "./review.ts";

/**
 * Rolling asynchronous Codex review.
 *
 * Delivery never waits for a Codex review. Every delivered Sentinel pull
 * request is reviewed later against its exact recorded identity, and the
 * completed review findings are ingested into the official review backlog as
 * normal future work. This module contains only deterministic, fail-closed
 * logic: pull-request eligibility, review-result validation, and backlog
 * ingestion. The async review execution lives in the worker entrypoint that
 * drives the pinned Codex CLI against the recorded head/base identities.
 */

export type RollingReviewPullState = "open" | "merged";

/** The minimal immutable pull-request identity the scan needs. */
export type SentinelPullRequest = Readonly<{
  number: number;
  htmlUrl: string;
  state: "open" | "closed";
  /** GitHub reports a closed pull request together with its merge result. */
  merged: boolean;
  headRef: string;
  headSha: string;
  baseRef: string;
  body: string;
}>;

export type RollingReviewParseStatus = "findings" | "no_findings" | "unparseable";

export type RollingReviewResultStatus = "completed" | "unparseable";

/**
 * Durable result of one completed (or fail-closed) rolling Codex review.
 * The record's identity (pull request number, exact head and base SHA, head
 * branch, and request id derived from the file name) is what later cycles use
 * to ingest findings without re-running the review.
 */
export type RollingReviewResult = Readonly<{
  schema_version: 1;
  /** Stable review identity: `<pr-number>-<head-sha>` (also the file stem). */
  request_id: string;
  pr_number: number;
  pr_url: string;
  head_sha: string;
  base_sha: string;
  head_branch: string;
  status: RollingReviewResultStatus;
  reviewed_at: string;
  parse_status: RollingReviewParseStatus;
  /** Final review text written by the pinned Codex CLI to stdout. */
  raw_review_text: string;
  /** Bounded diagnostics from the review invocation (progress only). */
  review_stderr: string;
  /** Structured ReviewOutputEvent from the review rollout, when retained. */
  structured_review: unknown | null;
  /** Parsed findings; always empty for an unparseable or no-findings record. */
  findings: readonly NativeReviewFinding[];
  /** Exact reason a record is unparseable; null for completed records. */
  failure: string | null;
}>;

export type RollingReviewIngestOutcome = Readonly<{
  record: RollingReviewResult;
  /** P0/P1 findings — mandatory remediation, surfaced ahead of P2/P3; they never gate the reviewed pull request merge. */
  priorityFindings: readonly NativeReviewFinding[];
  /** P2/P3 findings — ordinary backlog work. */
  backlogFindings: readonly NativeReviewFinding[];
  report: NativeReviewReport | null;
}>;

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_BRANCH = /^sentinel\/candidate-[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const SAFE_URL_PREFIX = /^https:\/\/github\.com\//u;
const SAFE_RESULT_NAME = /^[1-9][0-9]*-[0-9a-f]{40}\.json$/u;
const SEVERITIES = new Set<TriageSeverity>(["P0", "P1", "P2", "P3"]);
const VALID_PARSE_STATUSES = new Set<RollingReviewParseStatus>(["findings", "no_findings", "unparseable"]);

/** Maximum serialized bytes of a single durable review result. */
export const MAX_ROLLING_REVIEW_RESULT_BYTES = 256 * 1_024;
/** Maximum findings retained per completed review. */
export const MAX_ROLLING_REVIEW_FINDINGS = 100;
/** The finalized native review text and diagnostics are bounded. */
export const MAX_ROLLING_REVIEW_RAW_TEXT = 128 * 1_024;
export const MAX_ROLLING_REVIEW_RAW_TEXT_STDERR = 32 * 1_024;
export const MAX_ROLLING_REVIEW_FINDING_TITLE = 2_048;
export const MAX_ROLLING_REVIEW_FINDING_BODY = 32 * 1_024;
export const MAX_ROLLING_REVIEW_FINDING_LOCATION = 512;

const SENTINEL_PR_MARKERS = [
  "provider-sentinel-matrix:",
  "provider-sentinel:issue-pr:v1",
] as const;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isTimestamp = (value: unknown): value is string =>
  typeof value === "string" && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/u.test(value) &&
  Number.isFinite(Date.parse(value));

const isSeverity = (value: unknown): value is TriageSeverity =>
  typeof value === "string" && SEVERITIES.has(value as TriageSeverity);

const isSafeBranch = (value: string): boolean => SAFE_BRANCH.test(value);

/**
 * True when `value` is a structurally valid Sentinel pull request whose head
 * branch, marker, and base identity make it reviewable by the rolling worker.
 * A closed-but-unmerged pull request is never eligible: its diff is stale and
 * superseded by the next candidate generation.
 */
export const isSentinelRollingReviewPull = (value: unknown): value is SentinelPullRequest => {
  if (!isRecord(value)) return false;
  const pull = value as Record<string, unknown>;
  if (
    !Number.isSafeInteger(pull.number) || (pull.number as number) <= 0 ||
    typeof pull.htmlUrl !== "string" || !SAFE_URL_PREFIX.test(pull.htmlUrl) ||
    (pull.state !== "open" && pull.state !== "closed") ||
    typeof pull.merged !== "boolean" ||
    typeof pull.headRef !== "string" || !isSafeBranch(pull.headRef) ||
    typeof pull.headSha !== "string" || !FULL_SHA.test(pull.headSha) ||
    typeof pull.baseRef !== "string" || pull.baseRef !== "development" ||
    typeof pull.body !== "string"
  ) return false;
  const body = pull.body as string;
  if (pull.state === "open" && pull.merged) return false;
  if (pull.state === "closed" && !pull.merged) return false;
  return SENTINEL_PR_MARKERS.some((marker) => body.includes(marker));
};

/**
 * The durable result file name for one review identity. The stem doubles as
 * the review request id so later cycles can ingest findings by file name
 * without trusting unvalidated content.
 */
export const rollingReviewResultFileName = (prNumber: number, headSha: string): string => {
  if (!Number.isSafeInteger(prNumber) || prNumber <= 0 || !FULL_SHA.test(headSha)) {
    throw new Error("Rolling review result identity is invalid");
  }
  return `${prNumber}-${headSha}.json`;
};

export const rollingReviewRequestId = (prNumber: number, headSha: string): string => {
  const file = rollingReviewResultFileName(prNumber, headSha);
  return file.slice(0, -".json".length);
};

export const parseRollingReviewResultFileName = (name: string): { prNumber: number; headSha: string } | null => {
  if (!SAFE_RESULT_NAME.test(name)) return null;
  const stem = name.slice(0, -".json".length);
  const sep = stem.indexOf("-");
  if (sep <= 0) return null;
  const prNumber = Number(stem.slice(0, sep));
  const headSha = stem.slice(sep + 1);
  if (!Number.isSafeInteger(prNumber) || prNumber <= 0 || !FULL_SHA.test(headSha)) return null;
  return { prNumber, headSha };
};

const validateFinding = (value: unknown): NativeReviewFinding | null => {
  if (!isRecord(value)) return null;
  if (
    typeof value.fingerprint !== "string" || !/^[0-9a-f]{64}$/u.test(value.fingerprint) ||
    !isSeverity(value.severity) ||
    typeof value.title !== "string" || value.title.length === 0 ||
    value.title.length > MAX_ROLLING_REVIEW_FINDING_TITLE ||
    typeof value.body !== "string" || value.body.length > MAX_ROLLING_REVIEW_FINDING_BODY ||
    typeof value.location !== "string" || value.location.length > MAX_ROLLING_REVIEW_FINDING_LOCATION
  ) return null;
  return {
    fingerprint: value.fingerprint as string,
    severity: value.severity as TriageSeverity,
    title: value.title as string,
    body: value.body as string,
    location: value.location as string,
  };
};

/**
 * Fail-closed validated parse of one durable rolling review result. Every
 * identity field must match the exact file identity and the file name, and
 * the record must be internally consistent (unparseable ↔ no findings with
 * an exact failure; completed ↔ parseable and no failure). Any violation
 * throws and nothing is ever partially ingested.
 */
export const parseRollingReviewResult = (
  value: unknown,
  expected: Readonly<{ prNumber: number; headSha: string; fileName: string }>,
): RollingReviewResult => {
  if (
    !Number.isSafeInteger(expected.prNumber) || expected.prNumber <= 0 ||
    !FULL_SHA.test(expected.headSha)
  ) {
    throw new Error("Rolling review result identity is invalid");
  }
  const parsedName = parseRollingReviewResultFileName(expected.fileName);
  if (parsedName === null || parsedName.prNumber !== expected.prNumber || parsedName.headSha !== expected.headSha) {
    throw new Error("Rolling review result file name does not match its identity");
  }
  if (!isRecord(value)) throw new Error("Rolling review result is not a record");
  const raw = value as Record<string, unknown>;
  if (raw.schema_version !== 1) throw new Error("Rolling review result schema version is invalid");
  if (
    typeof raw.request_id !== "string" || raw.request_id !== rollingReviewRequestId(expected.prNumber, expected.headSha)
  ) {
    throw new Error("Rolling review result request id does not match its file identity");
  }
  if (raw.pr_number !== expected.prNumber) throw new Error("Rolling review result pull request number is invalid");
  if (typeof raw.pr_url !== "string" || !SAFE_URL_PREFIX.test(raw.pr_url)) {
    throw new Error("Rolling review result pull request URL is invalid");
  }
  if (typeof raw.head_sha !== "string" || !FULL_SHA.test(raw.head_sha) || raw.head_sha !== expected.headSha) {
    throw new Error("Rolling review result head SHA does not match its file identity");
  }
  if (typeof raw.base_sha !== "string" || !FULL_SHA.test(raw.base_sha) || raw.base_sha === raw.head_sha) {
    throw new Error("Rolling review result base SHA is invalid");
  }
  if (typeof raw.head_branch !== "string" || !isSafeBranch(raw.head_branch)) {
    throw new Error("Rolling review result head branch is invalid");
  }
  if (raw.status !== "completed" && raw.status !== "unparseable") {
    throw new Error("Rolling review result status is invalid");
  }
  if (typeof raw.reviewed_at !== "string" || !isTimestamp(raw.reviewed_at)) {
    throw new Error("Rolling review result reviewed-at timestamp is invalid");
  }
  if (typeof raw.parse_status !== "string" || !VALID_PARSE_STATUSES.has(raw.parse_status as RollingReviewParseStatus)) {
    throw new Error("Rolling review result parse status is invalid");
  }
  if (
    typeof raw.raw_review_text !== "string" ||
    new TextEncoder().encode(raw.raw_review_text).byteLength > MAX_ROLLING_REVIEW_RAW_TEXT
  ) {
    throw new Error("Rolling review result raw review text is invalid or too large");
  }
  if (
    typeof raw.review_stderr !== "string" ||
    new TextEncoder().encode(raw.review_stderr).byteLength > MAX_ROLLING_REVIEW_RAW_TEXT_STDERR
  ) {
    throw new Error("Rolling review result diagnostics are invalid or too large");
  }
  if (raw.structured_review !== null && !isRecord(raw.structured_review)) {
    throw new Error("Rolling review result structured review is invalid");
  }
  if (!Array.isArray(raw.findings) || raw.findings.length > MAX_ROLLING_REVIEW_FINDINGS) {
    throw new Error("Rolling review result findings are invalid or too numerous");
  }
  const findings = raw.findings.map(validateFinding);
  if (findings.some((finding) => finding === null)) {
    throw new Error("Rolling review result contains an invalid finding");
  }
  const parsedFindings = findings as NativeReviewFinding[];
  if (raw.failure !== null && typeof raw.failure !== "string") {
    throw new Error("Rolling review result failure is invalid");
  }
  if (raw.failure !== null && new TextEncoder().encode(raw.failure).byteLength > MAX_ROLLING_REVIEW_RAW_TEXT_STDERR) {
    throw new Error("Rolling review result failure is too large");
  }
  if (
    (raw.status === "unparseable") !== (raw.parse_status === "unparseable") ||
    (raw.parse_status === "unparseable" && (parsedFindings.length !== 0 || raw.failure === null)) ||
    (raw.parse_status !== "unparseable" && raw.failure !== null)
  ) {
    throw new Error("Rolling review result status is internally inconsistent");
  }
  if (
    (parsedFindings.length > 0) !== (raw.parse_status === "findings") ||
    (parsedFindings.length === 0 && raw.parse_status !== "no_findings" && raw.parse_status !== "unparseable")
  ) {
    throw new Error("Rolling review result findings do not match its parse status");
  }
  const serialized = JSON.stringify(value);
  if (new TextEncoder().encode(serialized).byteLength > MAX_ROLLING_REVIEW_RESULT_BYTES) {
    throw new Error("Rolling review result exceeds its byte limit");
  }
  return {
    schema_version: 1,
    request_id: raw.request_id as string,
    pr_number: raw.pr_number as number,
    pr_url: raw.pr_url as string,
    head_sha: raw.head_sha as string,
    base_sha: raw.base_sha as string,
    head_branch: raw.head_branch as string,
    status: raw.status as RollingReviewResultStatus,
    reviewed_at: raw.reviewed_at as string,
    parse_status: raw.parse_status as RollingReviewParseStatus,
    raw_review_text: raw.raw_review_text as string,
    review_stderr: raw.review_stderr as string,
    structured_review: raw.structured_review as unknown | null,
    findings: parsedFindings,
    failure: raw.failure as string | null,
  };
};

/**
 * Splits one validated record into P0/P1 findings and P2/P3 backlog findings.
 * All severities are non-blocking for the reviewed pull request's merge and
 * are ingested into the official backlog with their true severity. Unparseable
 * records produce no findings and no report: nothing is ingested and every
 * caller observes the fail-closed status.
 */
export const analyzeRollingReviewRecord = (record: RollingReviewResult): RollingReviewIngestOutcome => {
  if (record.status === "unparseable" || record.parse_status === "unparseable") {
    return { record, priorityFindings: [], backlogFindings: [], report: null };
  }
  const priorityFindings = record.findings.filter((finding) => finding.severity === "P0" || finding.severity === "P1");
  const backlogFindings = record.findings.filter((finding) => finding.severity === "P2" || finding.severity === "P3");
  const report: NativeReviewReport = {
    schema_version: 1,
    round: 1,
    parse_status: record.findings.length === 0 ? "no_findings" : "findings",
    findings: record.findings,
  };
  return { record, priorityFindings, backlogFindings, report };
};

/**
 * Ingests completed findings into the official review backlog, deduplicating
 * by fingerprint through the existing backlog merge. P0 and P1 findings enter
 * the backlog with their true severity so the normal selection order treats
 * them as mandatory remediation before P2 and P3. Unparseable records are
 * skipped here by design: they never became a completed review and their
 * exact evidence is retained in the durable result file.
 */
export const applyRollingReviewIngestion = (
  currentBacklogMarkdown: string,
  outcomes: readonly RollingReviewIngestOutcome[],
  observedAt: Date,
): string => {
  let markdown = currentBacklogMarkdown;
  for (const outcome of outcomes) {
    if (outcome.record.status === "unparseable" || outcome.report === null) continue;
    markdown = mergeReviewBacklog(markdown, outcome.report.findings, outcome.record.head_sha, observedAt);
  }
  return markdown;
};

export type RollingReviewScanResult = Readonly<{
  /** Eligible open or merged Sentinel pull requests. */
  eligible: readonly SentinelPullRequest[];
  /** Eligible pull requests without a completed or fail-closed result. */
  unreviewed: readonly SentinelPullRequest[];
  /** Reviewed pull requests paired with their validated durable results. */
  reviewed: readonly Readonly<{ pull: SentinelPullRequest; record: RollingReviewResult }>[];
  /** Fail-closed records that must never be silently re-reviewed or dropped. */
  unparseable: readonly RollingReviewResult[];
}>;

/**
 * Scans prior eligible open and merged Sentinel pull requests against the
 * validated durable review results. This is pure state inspection: it never
 * waits for a review, never blocks anything, and simply reports what is
 * outstanding and what has completed.
 */
export const scanRollingReviewResults = (
  pulls: readonly unknown[],
  results: readonly RollingReviewResult[],
): RollingReviewScanResult => {
  const eligible = pulls.filter(isSentinelRollingReviewPull);
  const byIdentity = new Map<string, RollingReviewResult>();
  for (const record of results) {
    const key = rollingReviewRequestId(record.pr_number, record.head_sha);
    if (byIdentity.has(key)) {
      throw new Error(`Rolling review result identity is duplicated: ${key}`);
    }
    byIdentity.set(key, record);
  }
  const reviewed: Array<Readonly<{ pull: SentinelPullRequest; record: RollingReviewResult }>> = [];
  const unreviewed: SentinelPullRequest[] = [];
  for (const pull of eligible) {
    const key = rollingReviewRequestId(pull.number, pull.headSha);
    const record = byIdentity.get(key);
    if (record === undefined) {
      unreviewed.push(pull);
      continue;
    }
    if (record.head_branch !== pull.headRef || record.head_sha !== pull.headSha) {
      throw new Error("Rolling review result identity drifted from its pull request");
    }
    reviewed.push({ pull, record });
  }
  const unparseable = results.filter((record) => record.status === "unparseable");
  return {
    eligible,
    unreviewed: unreviewed.sort((left, right) => left.number - right.number),
    reviewed,
    unparseable,
  };
};

/**
 * Selects the oldest eligible unreviewed Sentinel pull request (lowest pull
 * request number) for the asynchronous review worker, or null when nothing is
 * due. Returning null is not a failure: review latency is never a gate.
 */
export const selectNextRollingReviewTask = (
  pulls: readonly unknown[],
  results: readonly RollingReviewResult[],
): SentinelPullRequest | null => {
  const scan = scanRollingReviewResults(pulls, results);
  return scan.unreviewed[0] ?? null;
};

/** The durable review identity used by the file-name-only preselection. */
export type RollingReviewResultIdentity = Readonly<{
  prNumber: number;
  headSha: string;
}>;

/** Maximum durable rolling review result files accepted in one scan. */
export const MAX_ROLLING_REVIEW_RESULT_FILES = 256;

/**
 * Fail-closed identity parse of one `git ls-tree` listing of the durable
 * rolling review result directory. Every listed file must carry the exact
 * result identity in its name; any invalid or duplicated identity throws so
 * the caller surfaces the anomaly instead of silently skipping it. This is
 * the identity-level view of exactly the durable results the worker later
 * validates in full before ingestion.
 */
export const parseRollingReviewResultFileNames = (
  treeLines: readonly string[],
): readonly RollingReviewResultIdentity[] => {
  const prefix = `${SENTINEL_POLICY.paths.reviewResults}/`;
  const names = treeLines
    .filter((line) => line.length > 0)
    .filter((line) => line.startsWith(prefix))
    .map((line) => line.slice(prefix.length));
  if (names.length > MAX_ROLLING_REVIEW_RESULT_FILES) {
    throw new Error("Sentinel rolling review result directory exceeds its entry limit");
  }
  const identities: RollingReviewResultIdentity[] = [];
  const seen = new Set<string>();
  for (const name of names) {
    const identity = parseRollingReviewResultFileName(name);
    if (identity === null) {
      throw new Error(`Rolling review result file name is invalid: ${name}`);
    }
    const key = rollingReviewRequestId(identity.prNumber, identity.headSha);
    if (seen.has(key)) {
      throw new Error(`Rolling review result identity is duplicated: ${key}`);
    }
    seen.add(key);
    identities.push(identity);
  }
  return identities;
};

/**
 * Identity-only preselection of the oldest eligible unreviewed Sentinel pull
 * request, using exactly the same pull-eligibility and result-identity rules
 * as the full worker scan (one identity per `pr-number + head-sha`). This is
 * a bounded read-only check: it never invokes Codex and never waits for a
 * review, it only reports whether a review is due so the worker can be gated
 * independently of backlog or issue work. Returning null is not a failure:
 * review latency is never a gate.
 */
export const selectRollingReviewTaskFromIdentities = (
  pulls: readonly unknown[],
  resultIdentities: readonly RollingReviewResultIdentity[],
): SentinelPullRequest | null => {
  const reviewed = new Set<string>();
  for (const identity of resultIdentities) {
    reviewed.add(rollingReviewRequestId(identity.prNumber, identity.headSha));
  }
  const unreviewed = pulls
    .filter(isSentinelRollingReviewPull)
    .filter((pull) => !reviewed.has(rollingReviewRequestId(pull.number, pull.headSha)))
    .sort((left, right) => left.number - right.number);
  return unreviewed[0] ?? null;
};

/**
 * Parses the finalized native review text into a single finished report.
 * Both text and structured forms are supported, and every parse failure is
 * reported as unparseable instead of silently producing partial findings.
 */
export const parseCompletedRollingReview = async (
  input: Readonly<{
    rawReviewText: string;
    reviewStderr: string;
    structuredReview: unknown | null;
    checkoutPath: string;
    round?: number;
  }>,
): Promise<{ report: NativeReviewReport; parse_status: RollingReviewParseStatus; failure: string | null }> => {
  const round = input.round ?? 1;
  try {
    if (input.structuredReview !== null) {
      const report = await parseStructuredNativeReview(input.structuredReview, round, input.checkoutPath);
      return {
        report,
        parse_status: report.parse_status as RollingReviewParseStatus,
        failure: report.parse_status === "unparseable" ? "Structured Codex review output could not be parsed" : null,
      };
    }
    const report = await parseNativeReview(input.rawReviewText, round);
    return {
      report,
      parse_status: report.parse_status as RollingReviewParseStatus,
      failure: report.parse_status === "unparseable" ? "Native Codex review output could not be parsed" : null,
    };
  } catch (error) {
    return {
      report: { schema_version: 1, round, parse_status: "unparseable", findings: [] },
      parse_status: "unparseable",
      failure: error instanceof Error ? error.message : "Rolling review parse failed",
    };
  }
};

/** The raw review text and diagnostics are bounded before durable retention. */
export const boundedRawReview = (stdout: string, stderr: string): { stdout: string; stderr: string } => {
  const encoder = new TextEncoder();
  const truncate = (value: string, limit: number): string => {
    if (encoder.encode(value).byteLength <= limit) return value;
    return value.slice(0, limit);
  };
  return {
    stdout: truncate(stdout, MAX_ROLLING_REVIEW_RAW_TEXT),
    stderr: truncate(stderr, MAX_ROLLING_REVIEW_RAW_TEXT_STDERR),
  };
};
