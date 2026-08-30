import assert from "node:assert/strict";
import {
  analyzeRollingReviewRecord,
  applyRollingReviewIngestion,
  isSentinelRollingReviewPull,
  parseCompletedRollingReview,
  parseRollingReviewResult,
  parseRollingReviewResultFileName,
  parseRollingReviewResultFileNames,
  rollingReviewRequestId,
  type RollingReviewResult,
  rollingReviewResultFileName,
  scanRollingReviewResults,
  selectNextRollingReviewTask,
  selectRollingReviewTaskFromIdentities,
  type SentinelPullRequest,
} from "../scripts/sentinel/rolling-review.ts";
import { parseSentinelPullList } from "../scripts/sentinel/rolling-review-worker.ts";
import { SENTINEL_POLICY } from "../scripts/sentinel/policy.ts";
import { parseReviewBacklog, renderReviewBacklog, selectNextReviewBacklogEntry } from "../scripts/sentinel/review.ts";
import type { NativeReviewFinding } from "../scripts/sentinel/types.ts";

const FULL_SHA = "a".repeat(40);
const REVIEW_RESULT_PREFIX = `${SENTINEL_POLICY.paths.reviewResults}/`;

const finding = (
  fingerprint: string,
  severity: "P0" | "P1" | "P2" | "P3",
  location = "src/handler.ts:439",
): NativeReviewFinding => ({
  fingerprint,
  severity,
  title: `[${severity}] Review finding ${fingerprint.slice(0, 8)}`,
  body: `detail for ${fingerprint.slice(0, 8)} at ${location}`,
  location,
});

const push = (
  number: number,
  partial: Partial<SentinelPullRequest> = {},
): SentinelPullRequest => ({
  number,
  htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/pull/${number}`,
  state: "open",
  merged: false,
  headRef: `sentinel/candidate-${number}-1234567890abcdef`,
  headSha: `${FULL_SHA.slice(0, 36)}${number.toString(16).padStart(4, "0")}`,
  baseRef: "development",
  body: "<!-- provider-sentinel-matrix:run:1:HEAD --> candidate",
  ...partial,
});

const record = (
  prNumber: number,
  headSha: string,
  partial: Partial<RollingReviewResult> = {},
): RollingReviewResult => ({
  schema_version: 1,
  request_id: rollingReviewRequestId(prNumber, headSha),
  pr_number: prNumber,
  pr_url: `https://github.com/ubiquity/ai.ubq.fi/pull/${prNumber}`,
  head_sha: headSha,
  base_sha: "b".repeat(40),
  head_branch: `sentinel/candidate-${prNumber}-1234567890abcdef`,
  status: "completed",
  reviewed_at: "2026-08-22T00:00:00.000Z",
  parse_status: "no_findings",
  raw_review_text: "No findings.",
  review_stderr: "",
  structured_review: null,
  findings: [],
  failure: null,
  ...partial,
});

Deno.test("rolling review never waits and never gates on review completion", () => {
  // No outstanding, no completed: never a gate, never an error.
  assert.deepEqual(selectNextRollingReviewTask([], []), null);
  const eligibleOpen = push(101);
  const eligibleMerged = { ...push(102), state: "closed" as const, merged: true };
  const scan = scanRollingReviewResults([eligibleOpen, eligibleMerged], []);
  assert.deepEqual(scan.eligible.length, 2);
  assert.deepEqual(scan.unreviewed.map((entry) => entry.number), [101, 102]);
  assert.deepEqual(scan.reviewed.length, 0);
  // The oldest eligible unreviewed pull request is selected deterministically.
  const task = selectNextRollingReviewTask([eligibleOpen, eligibleMerged], []);
  assert.deepEqual(task?.number, 101);
  // Once completed, the same pull request is never re-reviewed and no
  // unreviewed work remains: returning null is a normal outcome.
  const completed = record(101, eligibleOpen.headSha, {
    parse_status: "findings",
    findings: [finding("1".repeat(64), "P2")],
  });
  assert.deepEqual(
    selectNextRollingReviewTask([eligibleOpen, eligibleMerged], [completed])?.number,
    102,
  );
  const after = scanRollingReviewResults([eligibleOpen, eligibleMerged], [completed]);
  assert.deepEqual(after.reviewed.length, 1);
  assert.deepEqual(after.unreviewed.map((entry) => entry.number), [102]);
});

Deno.test("rolling review ingestion is later and merges into the official backlog", async () => {
  const completed = record(203, "c".repeat(40), {
    parse_status: "findings",
    findings: [finding("2".repeat(64), "P2")],
  });
  const outcome = analyzeRollingReviewRecord(completed);
  assert.deepEqual(outcome.backlogFindings.length, 1);
  assert.deepEqual(outcome.priorityFindings.length, 0);
  const backlog = applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-22T01:00:00.000Z"));
  const entries = parseReviewBacklog(backlog);
  assert.deepEqual(entries.length, 1);
  assert.deepEqual(entries[0]!.fingerprint, "2".repeat(64));
  assert.deepEqual(entries[0]!.severity, "P2");
  assert.deepEqual(entries[0]!.disposition, "open");
  assert.deepEqual(entries[0]!.sha, "c".repeat(40));
  // The ingested entry is selected as normal future work.
  assert.deepEqual(selectNextReviewBacklogEntry(backlog)?.fingerprint, "2".repeat(64));
  // Finished review text from a later cycle parses and ingests too.
  const raw = `[P2] Fix later\nsrc/handler.ts:439 — description\n`;
  const parsed = await parseCompletedRollingReview({
    rawReviewText: raw,
    reviewStderr: "",
    structuredReview: null,
    checkoutPath: "/worktrees/candidate-worktree",
    round: 1,
  });
  assert.deepEqual(parsed.parse_status, "findings");
  assert.deepEqual(parsed.report.findings.length, 1);
});

Deno.test("rolling review deduplicates findings across prior pull requests", () => {
  const fingerprint = "3".repeat(64);
  const first = record(301, "d".repeat(40), {
    parse_status: "findings",
    findings: [finding(fingerprint, "P2")],
  });
  const second = record(302, "e".repeat(40), {
    parse_status: "findings",
    findings: [finding(fingerprint, "P2")],
  });
  const backlog = applyRollingReviewIngestion(
    renderReviewBacklog([]),
    [analyzeRollingReviewRecord(first), analyzeRollingReviewRecord(second)],
    new Date("2026-08-22T02:00:00.000Z"),
  );
  const entries = parseReviewBacklog(backlog);
  assert.deepEqual(entries.length, 1);
  assert.deepEqual(entries[0]!.first, "2026-08-22T02:00:00.000Z");
  assert.deepEqual(entries[0]!.latest, "2026-08-22T02:00:00.000Z");
  assert.deepEqual(entries[0]!.sha, "e".repeat(40));
  // Applying the same completed review a second time remains one entry.
  const repeated = applyRollingReviewIngestion(
    backlog,
    [analyzeRollingReviewRecord(second)],
    new Date("2026-08-22T03:00:00.000Z"),
  );
  assert.deepEqual(parseReviewBacklog(repeated).length, 1);
});

Deno.test("P0 and P1 findings are mandatory remediation sorted ahead of P2 and P3", () => {
  const outcome = analyzeRollingReviewRecord(record(401, "f".repeat(40), {
    parse_status: "findings",
    findings: [
      finding("4".repeat(64), "P0"),
      finding("5".repeat(64), "P1"),
      finding("6".repeat(64), "P2"),
      finding("7".repeat(64), "P3"),
    ],
  }));
  assert.deepEqual(outcome.priorityFindings.length, 2);
  assert.deepEqual(outcome.backlogFindings.length, 2);
  const backlog = applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-22T04:00:00.000Z"));
  const entries = parseReviewBacklog(backlog);
  assert.deepEqual(entries.map((entry) => entry.severity), ["P0", "P1", "P2", "P3"]);
  assert.deepEqual(selectNextReviewBacklogEntry(backlog)?.severity, "P0");
  const resolvedP0 = parseReviewBacklog(backlog).map((entry) =>
    entry.severity === "P0" ? { ...entry, disposition: "resolved" as const } : entry
  );
  const rest = renderReviewBacklog(resolvedP0);
  assert.deepEqual(selectNextReviewBacklogEntry(rest)?.severity, "P1");
  // P0 remains a P0 entry: its severity is never downgraded or dropped.
  assert(entries.some((entry) => entry.severity === "P0" && entry.finding.includes("P0")));
});

Deno.test("all severities are non-blocking: P0-P3 findings never gate the reviewed pull request merge", () => {
  // The reviewed pull request was merged with no review available at all.
  const merged = { ...push(411), state: "closed" as const, merged: true };
  const noReviewScan = scanRollingReviewResults([merged], []);
  assert.deepEqual(noReviewScan.eligible.length, 1);
  assert.deepEqual(noReviewScan.unreviewed.map((entry) => entry.number), [411]);
  // The later completed review found every severity, including P0. Every
  // finding is retained — nothing is dropped, downgraded, or treated as a
  // merge failure, and the review scan stays a plain reviewed outcome.
  const worstCase = record(411, merged.headSha, {
    parse_status: "findings",
    findings: [
      finding("a".repeat(64), "P0"),
      finding("b".repeat(64), "P1"),
      finding("c".repeat(64), "P2"),
      finding("d".repeat(64), "P3"),
    ],
  });
  const outcome = analyzeRollingReviewRecord(worstCase);
  assert.deepEqual(outcome.report?.findings.map((entry) => entry.severity), ["P0", "P1", "P2", "P3"]);
  assert.deepEqual(outcome.priorityFindings.map((entry) => entry.severity), ["P0", "P1"]);
  assert.deepEqual(outcome.backlogFindings.map((entry) => entry.severity), ["P2", "P3"]);
  const reviewed = scanRollingReviewResults([merged], [worstCase]);
  assert.deepEqual(reviewed.reviewed.length, 1);
  assert.deepEqual(reviewed.unreviewed.length, 0);
  // Nothing is pending after review, and no review outcome raises: the merge
  // of the reviewed pull request is never revisited or un-done by severity.
  assert.deepEqual(selectNextRollingReviewTask([merged], [worstCase]), null);
  const backlog = applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-22T04:30:00.000Z"));
  assert.deepEqual(
    parseReviewBacklog(backlog).map((entry) => entry.severity),
    ["P0", "P1", "P2", "P3"],
  );
  assert.match(backlog, /never block the reviewed pull request merge/u);
  // Selection is normal future work: P0 first, then P1, exactly once each.
  assert.deepEqual(selectNextReviewBacklogEntry(backlog)?.severity, "P0");
});

Deno.test("prior eligible open and merged Sentinel pull requests are scanned exactly", () => {
  const eligibleOpen = push(501);
  const eligibleMerged = { ...push(502), state: "closed" as const, merged: true };
  const closedUnmerged = { ...push(503), state: "closed" as const, merged: false };
  const otherBranch = { ...push(504), baseRef: "main" };
  const notSentinel = { ...push(505), body: "ordinary work" };
  const notCandidateHead = { ...push(506), headRef: "feat/agent-readiness" };
  for (const candidate of [eligibleOpen, eligibleMerged, closedUnmerged, otherBranch, notSentinel, notCandidateHead]) {
    assert.deepEqual(
      isSentinelRollingReviewPull(candidate),
      candidate === eligibleOpen || candidate === eligibleMerged,
    );
  }
  const completed = record(502, eligibleMerged.headSha, {
    parse_status: "findings",
    findings: [finding("8".repeat(64), "P2")],
  });
  const scan = scanRollingReviewResults(
    [eligibleOpen, eligibleMerged, closedUnmerged, otherBranch, notSentinel, notCandidateHead],
    [completed],
  );
  assert.deepEqual(scan.eligible.length, 2);
  assert.deepEqual(scan.reviewed.length, 1);
  assert.deepEqual(scan.unreviewed.map((entry) => entry.number), [501]);
  // A completed review whose identity drifts from its pull request is fail-closed.
  const drifted = { ...completed, head_branch: "sentinel/candidate-999-nosuchbranch" };
  assert.throws(() => scanRollingReviewResults([eligibleMerged], [drifted]), /drifted/);
});

Deno.test("rolling review result files enforce exact identity and fail closed on malformed data", async () => {
  const prNumber = 601;
  const headSha = "1a".repeat(20);
  const fileName = rollingReviewResultFileName(prNumber, headSha);
  assert.deepEqual(rollingReviewRequestId(prNumber, headSha), fileName.replace(/\.json$/u, ""));
  assert.deepEqual(parseRollingReviewResultFileName(fileName), { prNumber, headSha });
  assert.deepEqual(parseRollingReviewResultFileName("not-a-result.json"), null);
  assert.deepEqual(parseRollingReviewResultFileName("0-${headSha}.json"), null);

  const valid = record(prNumber, headSha, {
    parse_status: "findings",
    findings: [finding("9".repeat(64), "P2")],
  });
  const parsed = parseRollingReviewResult(valid, { prNumber, headSha, fileName });
  assert.deepEqual(parsed.request_id, rollingReviewRequestId(prNumber, headSha));
  assert.throws(
    () => parseRollingReviewResult({ ...valid, pr_number: prNumber + 1 }, { prNumber, headSha, fileName }),
    /pull request number/,
  );
  assert.throws(
    () => parseRollingReviewResult({ ...valid, head_sha: "ab".repeat(20) }, { prNumber, headSha, fileName }),
    /head SHA/,
  );
  assert.throws(
    () => parseRollingReviewResult({ ...valid, request_id: "wrong" }, { prNumber, headSha, fileName }),
    /request id/,
  );
  // Status consistency is enforced in both directions.
  assert.throws(
    () => parseRollingReviewResult({ ...valid, status: "unparseable", failure: null }, { prNumber, headSha, fileName }),
    /internally inconsistent/,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...valid, parse_status: "unparseable", findings: [] }, {
        prNumber,
        headSha,
        fileName,
      }),
    /internally inconsistent/,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...valid, parse_status: "no_findings", findings: [finding("9".repeat(64), "P2")] }, {
        prNumber,
        headSha,
        fileName,
      }),
    /do not match/,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...valid, findings: [{ ...finding("9".repeat(64), "P2"), severity: "P7" }] }, {
        prNumber,
        headSha,
        fileName,
      }),
    /invalid finding/,
  );
  // A malformed review parses fail-closed: no findings, an exact failure.
  const malformed = await parseCompletedRollingReview({
    rawReviewText: "[P2]\nsome text without a priority marker",
    reviewStderr: "",
    structuredReview: null,
    checkoutPath: "/worktrees/candidate-worktree",
  });
  assert.deepEqual(malformed.parse_status, "unparseable");
  assert.notEqual(malformed.failure, null);
  const badOutcome = analyzeRollingReviewRecord(record(prNumber, headSha, {
    status: "unparseable",
    parse_status: "unparseable",
    findings: [],
    failure: "review output was malformed",
  }));
  assert.deepEqual(badOutcome.priorityFindings.length, 0);
  assert.deepEqual(badOutcome.backlogFindings.length, 0);
  assert.deepEqual(badOutcome.report, null);
  assert.deepEqual(
    applyRollingReviewIngestion(renderReviewBacklog([]), [badOutcome], new Date("2026-08-22T05:00:00.000Z")),
    renderReviewBacklog([]),
  );
});

Deno.test("rolling review structured findings parse fail-closed and preserve severity", async () => {
  const checkoutPath = "/worktrees/candidate-worktree";
  const structured = {
    findings: [{
      title: "[P1] Clear the original replay body after snapshotting",
      body: "detail",
      confidence_score: 0.9,
      priority: 1,
      code_location: {
        absolute_file_path: `${checkoutPath}/src/handler.ts`,
        line_range: { start: 430, end: 433 },
      },
    }],
    overall_correctness: "patch is incorrect",
    overall_explanation: "One blocking finding.",
    overall_confidence_score: 0.8,
  };
  const parsed = await parseCompletedRollingReview({
    rawReviewText: "",
    reviewStderr: "",
    structuredReview: structured,
    checkoutPath,
    round: 1,
  });
  assert.deepEqual(parsed.parse_status, "findings");
  assert.deepEqual(parsed.report.findings[0]!.severity, "P1");
  const outcome = analyzeRollingReviewRecord(record(701, "ab".repeat(20), {
    parse_status: "findings",
    findings: parsed.report.findings,
  }));
  assert.deepEqual(outcome.priorityFindings.length, 1);
  const backlog = applyRollingReviewIngestion(
    renderReviewBacklog([]),
    [outcome],
    new Date("2026-08-22T06:00:00.000Z"),
  );
  assert.deepEqual(selectNextReviewBacklogEntry(backlog)?.severity, "P1");
  // Structured output that contradicts itself is unparseable, never partially trusted.
  const selfContradictory = {
    ...structured,
    findings: [],
    overall_correctness: "patch is incorrect",
  };
  const broken = await parseCompletedRollingReview({
    rawReviewText: "",
    reviewStderr: "",
    structuredReview: selfContradictory,
    checkoutPath,
    round: 1,
  });
  assert.deepEqual(broken.parse_status, "unparseable");
});

Deno.test("rolling review preselection runs review-only work when no backlog or issue work exists", () => {
  // The identity-only preselection never consults backlog or issue state: an
  // empty result directory plus eligible open and merged Sentinel pull
  // requests is enough to declare review work due.
  const eligibleOpen = push(811);
  const eligibleMerged = { ...push(812), state: "closed" as const, merged: true };
  const notSentinel = { ...push(813), body: "ordinary work" };
  assert.deepEqual(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged, notSentinel], []),
    eligibleOpen,
  );
  // A merged Sentinel pull request with a durable result identity is not
  // re-reviewed; the next oldest eligible pull request becomes the task.
  const mergedReviewed = parseRollingReviewResultFileNames([
    `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(812, eligibleMerged.headSha)}`,
  ]);
  assert.deepEqual(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged, notSentinel], mergedReviewed),
    eligibleOpen,
  );
  // The identity-only preselection agrees exactly with the full worker scan
  // over the same pulls and the equivalent durable records.
  const completed = record(812, eligibleMerged.headSha, {
    parse_status: "findings",
    findings: [finding("a1".repeat(32), "P2")],
  });
  const fullTask = selectNextRollingReviewTask([eligibleOpen, eligibleMerged, notSentinel], [completed]);
  assert.deepEqual(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged, notSentinel], mergedReviewed),
    fullTask,
  );
});

Deno.test("rolling review preselection reports no review due only when every eligible pull is reviewed", () => {
  const eligibleOpen = push(821);
  const eligibleMerged = { ...push(822), state: "closed" as const, merged: true };
  const closedUnmerged = { ...push(823), state: "closed" as const, merged: false };
  const allReviewed = parseRollingReviewResultFileNames([
    `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(821, eligibleOpen.headSha)}`,
    `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(822, eligibleMerged.headSha)}`,
    // A closed-unmerged pull request is not eligible, so no result is required
    // for it; its presence must never make a review appear due.
  ]);
  assert.equal(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged, closedUnmerged], allReviewed),
    null,
  );
  // No pulls and no results is the quietest possible state: still no review.
  assert.equal(selectRollingReviewTaskFromIdentities([], []), null);
  // Every existing PR result leaves a review due for each missing identity.
  assert.equal(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged], allReviewed.slice(0, -1))?.number,
    822,
  );
  assert.equal(
    selectRollingReviewTaskFromIdentities([eligibleOpen, eligibleMerged], allReviewed.slice(1))?.number,
    821,
  );
});

Deno.test("rolling review result file identity parse is fail-closed and bounded", () => {
  const headSha = "1b".repeat(20);
  assert.deepEqual(
    parseRollingReviewResultFileNames([
      `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(911, headSha)}`,
    ]),
    [{ prNumber: 911, headSha }],
  );
  // Lines outside the durable result directory are ignored exactly like the
  // worker scan; empty lines are harmless.
  assert.deepEqual(parseRollingReviewResultFileNames([`docs/sentinel-review-results-bak/1-${headSha}.json`, ""]), []);
  // Any invalid identity fails closed: the anomaly is surfaced instead of
  // silently treated as reviewed (which would starve the review worker).
  assert.throws(
    () => parseRollingReviewResultFileNames([`${REVIEW_RESULT_PREFIX}not-a-result.json`]),
    /file name is invalid/,
  );
  const duplicate = `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(912, headSha)}`;
  assert.throws(
    () => parseRollingReviewResultFileNames([duplicate, duplicate]),
    /duplicated/,
  );
  // The entry limit mirrors the worker's bounded scan.
  const many = Array.from(
    { length: 257 },
    (_, index) => `${REVIEW_RESULT_PREFIX}${rollingReviewResultFileName(1_000 + index, headSha)}`,
  );
  assert.throws(() => parseRollingReviewResultFileNames(many), /exceeds its entry limit/);
});

Deno.test("rolling review listing accepts the live GitHub payload contract and preserves exact identity", () => {
  // Records captured from the live `GET /repos/ubiquity/ai.ubq.fi/pulls?state=all`
  // listing. The GitHub REST API reports an absent pull-request description as
  // `body: null` (PR #169 and #159); the pre-fix listing rejected that valid
  // contract value with "GitHub returned an invalid pull request" and aborted
  // the whole rolling review cycle.
  const liveMergedWithoutBody = {
    number: 169,
    html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/169",
    state: "closed",
    merged_at: "2026-08-28T09:08:28Z",
    body: null,
    head: { ref: "fix/sentinel-luna-stream-stall", sha: "f508d765147a7c260ee3bfe63a257dc062df40e0" },
    base: { ref: "development" },
  };
  const liveClosedUnmergedWithoutBody = {
    number: 159,
    html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/159",
    state: "closed",
    merged_at: null,
    body: null,
    head: { ref: "fix/sentinel-hourly-selection", sha: "9105bffd073a002e09f43d579da5c3f09b23669b" },
    base: { ref: "development" },
  };
  const liveSentinelPull = {
    number: 166,
    html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/166",
    state: "closed",
    merged_at: "2026-08-28T03:32:40Z",
    body:
      "<!-- provider-sentinel:issue-pr:v1 issue=142 fingerprint=d8e960924dc9d21789c0f08da036b9521117aaa10f909cdffab7e62560887438 -->",
    head: { ref: "sentinel/candidate-33137768977", sha: "8870fadd42b55333a313cdfde2b5d7c540e3a731" },
    base: { ref: "development" },
  };
  const parsed = parseSentinelPullList([liveMergedWithoutBody, liveClosedUnmergedWithoutBody, liveSentinelPull]);
  assert.equal(parsed.length, 3);
  assert.deepEqual(parsed[0], {
    number: 169,
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/169",
    state: "closed",
    merged: true,
    headRef: "fix/sentinel-luna-stream-stall",
    headSha: "f508d765147a7c260ee3bfe63a257dc062df40e0",
    baseRef: "development",
    body: "",
  });
  assert.deepEqual(parsed[1], {
    number: 159,
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/159",
    state: "closed",
    merged: false,
    headRef: "fix/sentinel-hourly-selection",
    headSha: "9105bffd073a002e09f43d579da5c3f09b23669b",
    baseRef: "development",
    body: "",
  });
  // The exact Sentinel pull identity is preserved untouched for later
  // eligibility, and a bodyless non-Sentinel pull request is simply not
  // eligible rather than a parse failure.
  assert.deepEqual(parsed[2], {
    number: 166,
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/166",
    state: "closed",
    merged: true,
    headRef: "sentinel/candidate-33137768977",
    headSha: "8870fadd42b55333a313cdfde2b5d7c540e3a731",
    baseRef: "development",
    body: liveSentinelPull.body,
  });
  assert.equal(isSentinelRollingReviewPull(parsed[0]), false);
  assert.equal(isSentinelRollingReviewPull(parsed[1]), false);
  assert.equal(isSentinelRollingReviewPull(parsed[2]), true);
});

Deno.test("rolling review listing stays fail-closed for truly malformed pull records", () => {
  const valid = {
    number: 212,
    html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/212",
    state: "closed",
    merged_at: "2026-08-30T20:23:06Z",
    body: "<!-- provider-sentinel:issue-pr:v1 issue=142 -->",
    head: { ref: "sentinel/candidate-33137768977", sha: "8870fadd42b55333a313cdfde2b5d7c540e3a731" },
    base: { ref: "development" },
  };
  const malformed: unknown[] = [
    { ...valid, number: 0 },
    { ...valid, number: 1.5 },
    { ...valid, html_url: "https://example.com/pull/1" },
    { ...valid, state: "draft" },
    { ...valid, merged_at: 123 },
    { ...valid, body: 42 },
    { ...valid, head: null },
    { ...valid, head: { ref: "sentinel/candidate-x", sha: "short" } },
    { ...valid, head: { ref: "../escape", sha: "8870fadd42b55333a313cdfde2b5d7c540e3a731" } },
    { ...valid, base: null },
    { ...valid, base: { ref: "" } },
    null,
    "not-a-record",
  ];
  for (const candidate of malformed) {
    assert.throws(
      () => parseSentinelPullList([candidate]),
      /invalid pull request/,
      `malformed record must fail closed: ${JSON.stringify(candidate)?.slice(0, 80)}`,
    );
  }
  // A non-array page and a partially malformed page are equally fail-closed.
  assert.throws(() => parseSentinelPullList({ not: "a list" }), /listing is invalid/);
  assert.throws(
    () => parseSentinelPullList([{ ...valid, state: "open" }, { ...valid, head: null }]),
    /invalid pull request/,
  );
});
