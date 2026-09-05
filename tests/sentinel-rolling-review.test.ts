import assert from "node:assert/strict";
import {
  analyzeRollingReviewRecord,
  applyRollingReviewIngestion,
  isSentinelRollingReviewPull,
  MAX_ROLLING_REVIEW_RAW_TEXT_STDERR,
  parseCompletedRollingReview,
  parseRollingReviewResult,
  parseRollingReviewResultFileName,
  parseRollingReviewResultFileNames,
  resolveRollingReviewTargetAnchor,
  rollingReviewRequestId,
  type RollingReviewResult,
  rollingReviewResultFileName,
  scanRollingReviewResults,
  selectNextRollingReviewTask,
  selectRollingReviewTaskFromIdentities,
  type SentinelPullRequest,
  unreachableRollingReviewRecord,
} from "../scripts/sentinel/rolling-review.ts";
import {
  deferGitleaksRejectedTarget,
  GITLEAKS_TARGET_REJECTION,
  parseSentinelPullList,
  stageRollingReviewResult,
} from "../scripts/sentinel/rolling-review-worker.ts";
import { SENTINEL_POLICY } from "../scripts/sentinel/policy.ts";
import { parseReviewBacklog, renderReviewBacklog, selectNextReviewBacklogEntry } from "../scripts/sentinel/review.ts";
import type { NativeReviewFinding } from "../scripts/sentinel/types.ts";
import { runTrustedGit } from "../scripts/sentinel/validation.ts";
import backlogMarkdown from "../docs/sentinel-review-backlog.md" with { type: "text" };

const FULL_SHA = "a".repeat(40);
const REVIEW_RESULT_PREFIX = `${SENTINEL_POLICY.paths.reviewResults}/`;
const gitPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run", command: "git" }),
]);

const git = async (cwd: string, args: readonly string[]): Promise<string> =>
  new TextDecoder().decode((await runTrustedGit({ cwd, args })).stdout).trim();

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

Deno.test({
  name: "rolling review worker publishes finding results only and defers backlog ingestion",
  ignore: gitPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-rolling-review-worker-" });
    try {
      await runTrustedGit({ cwd: root, args: ["init", "-b", "development"] });
      const backlogPath = SENTINEL_POLICY.paths.reviewBacklog;
      const initialBacklog = renderReviewBacklog([]);
      await Deno.mkdir(`${root}/docs`, { recursive: true });
      await Deno.writeTextFile(`${root}/${backlogPath}`, initialBacklog);
      await runTrustedGit({ cwd: root, args: ["add", "--", backlogPath] });
      await runTrustedGit({ cwd: root, args: ["commit", "-m", "seed rolling review backlog"] });

      const headSha = "c".repeat(40);
      const fingerprint = "d".repeat(64);
      const reviewed = record(203, headSha, {
        parse_status: "findings",
        findings: [finding(fingerprint, "P2")],
      });
      const resultPath = await stageRollingReviewResult(root, reviewed);
      await runTrustedGit({ cwd: root, args: ["commit", "-m", "record rolling review result"] });

      const changed = (await runTrustedGit({
        cwd: root,
        args: ["diff-tree", "--no-commit-id", "--name-status", "--no-renames", "-r", "-z", "HEAD"],
      })).stdout;
      assert.deepEqual(new TextDecoder().decode(changed).split("\0").filter((entry) => entry.length > 0), [
        "A",
        resultPath,
      ]);
      assert.equal(await git(root, ["show", `HEAD:${backlogPath}`]), initialBacklog.trim());

      const persisted = JSON.parse(await git(root, ["show", `HEAD:${resultPath}`]));
      const parsed = parseRollingReviewResult(persisted, {
        prNumber: reviewed.pr_number,
        headSha: reviewed.head_sha,
        fileName: resultPath.slice(`${REVIEW_RESULT_PREFIX}`.length),
      });
      const outcome = analyzeRollingReviewRecord(parsed);
      assert.deepEqual(outcome.backlogFindings.map((entry) => entry.fingerprint), [fingerprint]);
      const ingested = applyRollingReviewIngestion(initialBacklog, [outcome], new Date("2026-08-31T00:00:00.000Z"));
      assert.deepEqual(parseReviewBacklog(ingested).map((entry) => entry.fingerprint), [fingerprint]);
      assert.equal(selectNextReviewBacklogEntry(ingested)?.fingerprint, fingerprint);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
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

Deno.test("rolling review git merge-base exit 1 becomes a durable unreachable disposition", () => {
  const livePr147Head = "8203788b929a9270e2d0c32ae60919bda185cdab";
  const liveDevelopment = "b1e88fc40ac8caf6ffb41c3e73a91d42cb79224b";
  // The live historical candidate: PR #147 at 8203788b929a... (fast-forward
  // merged on 2026-08-27, branch `sentinel/candidate-33035267454`), selected
  // as the oldest eligible unreviewed target by the rolling worker.
  // `git merge-base <head> origin/development` exited 1 with EMPTY stderr —
  // git's exact no-common-ancestor contract — which previously raised through
  // `runTrustedGit` (exit 1) and failed the whole non-blocking review step.
  const noAncestor = resolveRollingReviewTargetAnchor({ code: 1, stdout: "", stderr: "" }, livePr147Head);
  assert.equal(noAncestor.status, "unreachable");
  assert.match(noAncestor.failure, /exited 1 without a common ancestor/u);
  assert.match(noAncestor.failure, /no stderr output/u);
  // Any real git diagnostics are retained exactly and bounded to the durable
  // record's diagnostic limit.
  const withStderr = resolveRollingReviewTargetAnchor(
    { code: 1, stdout: "", stderr: "fatal: not a valid object name\n" },
    livePr147Head,
  );
  assert.equal(withStderr.status, "unreachable");
  assert.match(withStderr.failure, /not a valid object name/u);
  const huge = resolveRollingReviewTargetAnchor(
    { code: 1, stdout: "", stderr: "x".repeat(MAX_ROLLING_REVIEW_RAW_TEXT_STDERR * 4) },
    livePr147Head,
  );
  assert.equal(huge.status, "unreachable");
  // Byte-accurate truncation: high-byte diagnostics stay within the durable
  // record's strict size limit, so the record validates (never fail-closed).
  const multibyte = resolveRollingReviewTargetAnchor(
    { code: 1, stdout: "", stderr: "é".repeat(MAX_ROLLING_REVIEW_RAW_TEXT_STDERR * 2) },
    livePr147Head,
  );
  assert.equal(multibyte.status, "unreachable");
  const multibyteRecord = unreachableRollingReviewRecord({
    prNumber: 147,
    prUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/147",
    headSha: livePr147Head,
    headRef: "sentinel/candidate-33035267454",
    failure: multibyte.failure,
    observedAt: "2026-08-30T23:18:39.000Z",
  });
  assert.ok(new TextEncoder().encode(multibyteRecord.failure ?? "").byteLength <= MAX_ROLLING_REVIEW_RAW_TEXT_STDERR);
  assert.equal(multibyteRecord.findings.length, 0);
  assert.equal(
    unreachableRollingReviewRecord({
      prNumber: 147,
      prUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/147",
      headSha: livePr147Head,
      headRef: "sentinel/candidate-33035267454",
      failure: huge.failure,
      observedAt: "2026-08-30T23:18:39.000Z",
    }).findings.length,
    0,
  );
  // A successful merge base with a distinct base SHA anchors the review.
  assert.deepEqual(
    resolveRollingReviewTargetAnchor({ code: 0, stdout: `${liveDevelopment}\n`, stderr: "" }, livePr147Head),
    { status: "anchored", baseSha: liveDevelopment },
  );
  // A zero exit resolving no well-formed base is never trusted.
  const malformed = resolveRollingReviewTargetAnchor({ code: 0, stdout: "", stderr: "" }, livePr147Head);
  assert.equal(malformed.status, "unreachable");
  assert.match(malformed.failure, /no well-formed base SHA/u);
  // The target head identity itself still fails closed before any disposition.
  assert.throws(
    () => resolveRollingReviewTargetAnchor({ code: 0, stdout: liveDevelopment, stderr: "" }, "not-a-sha"),
    /head SHA is invalid/,
  );
});

Deno.test("rolling review live fast-forward candidate shape is retained as unreachable, never completed", () => {
  const livePr147 = {
    number: 147,
    htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/pull/147",
    headSha: "8203788b929a9270e2d0c32ae60919bda185cdab",
    headRef: "sentinel/candidate-33035267454",
  };
  const livePr148 = {
    ...push(148),
    state: "closed" as const,
    merged: true,
    headRef: "sentinel/candidate-33037193821",
    headSha: "91c65cbdebf9ffc1073c10cba7f5d12dc7a4cd2d",
  };
  // Every eligible live candidate head was merged by fast-forward
  // (`merge_commit_sha == head_sha`), so on a full clone git merge-base
  // returns the head itself; the old guard turned that into a fatal
  // "no exact merge base" worker failure and reselected the same PR forever.
  const selfBase = resolveRollingReviewTargetAnchor(
    { code: 0, stdout: `${livePr147.headSha}\n`, stderr: "" },
    livePr147.headSha,
  );
  assert.equal(selfBase.status, "unreachable");
  assert.match(selfBase.failure, /merge base is the head itself/u);
  const record = unreachableRollingReviewRecord({
    prNumber: livePr147.number,
    prUrl: livePr147.htmlUrl,
    headSha: livePr147.headSha,
    headRef: livePr147.headRef,
    failure: selfBase.failure,
    observedAt: "2026-08-30T23:18:39.000Z",
  });
  // Exact identity is preserved and the record validates under the same
  // strict rules later cycles apply.
  const fileName = rollingReviewResultFileName(livePr147.number, livePr147.headSha);
  assert.equal(record.request_id, rollingReviewRequestId(livePr147.number, livePr147.headSha));
  assert.equal(record.status, "unreachable");
  assert.equal(record.parse_status, "unreachable");
  assert.equal(record.base_sha, null);
  assert.deepEqual(record.findings, []);
  assert.match(record.failure ?? "", /merge base is the head itself/u);
  const parsed = parseRollingReviewResult(record, {
    prNumber: livePr147.number,
    headSha: livePr147.headSha,
    fileName,
  });
  assert.equal(parsed.status, "unreachable");
  assert.deepEqual(parseRollingReviewResultFileName(fileName), {
    prNumber: livePr147.number,
    headSha: livePr147.headSha,
  });
  // The disposition is durable: the target is consumed by the scan and the
  // identity-only preselection exactly like a completed result, and no
  // completed review is ever claimed.
  const pull147: SentinelPullRequest = {
    ...push(livePr147.number),
    state: "closed",
    merged: true,
    headRef: livePr147.headRef,
    headSha: livePr147.headSha,
    htmlUrl: livePr147.htmlUrl,
  };
  const scan = scanRollingReviewResults([pull147, livePr148], [record]);
  assert.deepEqual(scan.reviewed.map((entry) => entry.pull.number), [147]);
  assert.deepEqual(scan.unreviewed.map((entry) => entry.number), [148]);
  assert.deepEqual(selectNextRollingReviewTask([pull147, livePr148], [record])?.number, 148);
  const identities = parseRollingReviewResultFileNames([
    `${REVIEW_RESULT_PREFIX}${fileName}`,
  ]);
  assert.deepEqual(
    selectRollingReviewTaskFromIdentities([pull147, livePr148], identities)?.number,
    148,
  );
  // Never ingested into the official backlog: no findings, no report.
  const outcome = analyzeRollingReviewRecord(record);
  assert.deepEqual(outcome.priorityFindings, []);
  assert.deepEqual(outcome.backlogFindings, []);
  assert.equal(outcome.report, null);
  assert.equal(
    applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-30T23:19:00.000Z")),
    renderReviewBacklog([]),
  );
  // Fail-closed: an unreachable record never masquerades as completed, and
  // completed records never lose their exact base.
  assert.throws(
    () =>
      parseRollingReviewResult({ ...record, base_sha: "b".repeat(40) }, {
        prNumber: 147,
        headSha: livePr147.headSha,
        fileName,
      }),
    /base SHA must be null/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...record, failure: null }, { prNumber: 147, headSha: livePr147.headSha, fileName }),
    /internally inconsistent/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult(
        { ...record, status: "completed", parse_status: "unreachable", base_sha: "b".repeat(40), failure: null },
        { prNumber: 147, headSha: livePr147.headSha, fileName },
      ),
    /internally inconsistent/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({
        ...record,
        findings: [finding("b1".repeat(32), "P2")],
      }, { prNumber: 147, headSha: livePr147.headSha, fileName }),
    /internally inconsistent/u,
  );
  const completed = parseRollingReviewResult(
    { ...record, status: "completed", parse_status: "no_findings", base_sha: "b".repeat(40), failure: null },
    { prNumber: 147, headSha: livePr147.headSha, fileName },
  );
  assert.equal(completed.status, "completed");
  assert.throws(
    () =>
      parseRollingReviewResult({ ...completed, base_sha: null }, {
        prNumber: 147,
        headSha: livePr147.headSha,
        fileName,
      }),
    /base SHA is invalid/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...completed, base_sha: livePr147.headSha }, {
        prNumber: 147,
        headSha: livePr147.headSha,
        fileName,
      }),
    /base SHA is invalid/u,
  );
  // An unparseable record still requires its own exact base: nothing weakened.
  assert.throws(
    () =>
      parseRollingReviewResult(
        { ...record, status: "unparseable", parse_status: "unparseable", base_sha: null },
        { prNumber: 147, headSha: livePr147.headSha, fileName },
      ),
    /base SHA is invalid/u,
  );
  // Identity drift between the durable record and its pull request stays
  // fail-closed for unreachable dispositions too.
  assert.throws(
    () => scanRollingReviewResults([pull147], [{ ...record, head_branch: "sentinel/candidate-999-nosuchbranch" }]),
    /drifted/,
  );
});

Deno.test("rolling review anchored Gitleaks rejection is the strict rejected disposition", () => {
  // `scanCandidateWithGitleaks` throws exactly this error object when it
  // rejects the candidate working tree or Git history, and the worker must
  // catch only this target-validation rejection.
  const task = push(931);
  const baseSha = "b".repeat(40);
  const observedAt = "2026-08-31T00:00:00.000Z";
  const scanFailure = new Error(GITLEAKS_TARGET_REJECTION);
  const deferred = deferGitleaksRejectedTarget(
    scanFailure,
    { prNumber: task.number, prUrl: task.htmlUrl, headSha: task.headSha, headRef: task.headRef },
    baseSha,
    observedAt,
  );
  // The disposition keeps the same exact PR/head identity as the selected
  // target and passes the same strict contract later cycles apply.
  const fileName = rollingReviewResultFileName(task.number, task.headSha);
  assert.equal(deferred.pr_number, task.number);
  assert.equal(deferred.pr_url, task.htmlUrl);
  assert.equal(deferred.head_sha, task.headSha);
  assert.equal(deferred.head_branch, task.headRef);
  assert.equal(deferred.request_id, rollingReviewRequestId(task.number, task.headSha));
  // The target was successfully anchored before the gate rejected it, so the
  // exact proven merge base is preserved: never base_sha:null, and never the
  // unanchored unreachable semantics.
  assert.equal(deferred.base_sha, baseSha);
  assert.equal(
    parseRollingReviewResult(deferred, { prNumber: task.number, headSha: task.headSha, fileName }).status,
    "rejected",
  );
  // It is the non-complete fail-closed disposition, never a completed review,
  // and it carries the exact safe rejection reason with zero findings.
  assert.notEqual(deferred.status, "completed");
  assert.equal(deferred.status, "rejected");
  assert.equal(deferred.parse_status, "rejected");
  assert.equal(deferred.failure, GITLEAKS_TARGET_REJECTION);
  assert.deepEqual(deferred.findings, []);
  assert.equal(deferred.raw_review_text, "");
  assert.equal(deferred.review_stderr, "");
  assert.equal(deferred.structured_review, null);
  // A malformed rejected record fails closed exactly like every other
  // disposition: the exact base can never be dropped, the failure can never
  // be erased, findings can never be attached, and status/parse status can
  // never diverge.
  assert.throws(
    () =>
      parseRollingReviewResult({ ...deferred, base_sha: null }, {
        prNumber: task.number,
        headSha: task.headSha,
        fileName,
      }),
    /base SHA is invalid/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...deferred, failure: null }, {
        prNumber: task.number,
        headSha: task.headSha,
        fileName,
      }),
    /internally inconsistent/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...deferred, findings: [finding("c1".repeat(32), "P2")] }, {
        prNumber: task.number,
        headSha: task.headSha,
        fileName,
      }),
    /internally inconsistent/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...deferred, parse_status: "no_findings" }, {
        prNumber: task.number,
        headSha: task.headSha,
        fileName,
      }),
    /internally inconsistent/u,
  );
  assert.throws(
    () =>
      parseRollingReviewResult({ ...deferred, base_sha: task.headSha }, {
        prNumber: task.number,
        headSha: task.headSha,
        fileName,
      }),
    /base SHA is invalid/u,
  );
  const outcome = analyzeRollingReviewRecord(deferred);
  assert.deepEqual(outcome.priorityFindings, []);
  assert.deepEqual(outcome.backlogFindings, []);
  assert.equal(outcome.report, null);
  assert.equal(
    applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-31T00:00:01.000Z")),
    renderReviewBacklog([]),
  );
  // The target is consumed exactly like every other durable disposition, so
  // the worker never re-selects it: the next cycle reports no pending review
  // for the target and the worker exits successfully instead of failing.
  const scan = scanRollingReviewResults([task], [deferred]);
  assert.deepEqual(scan.reviewed.map((entry) => entry.pull.number), [931]);
  assert.deepEqual(scan.unreviewed, []);
  assert.equal(selectNextRollingReviewTask([task], [deferred]), null);
  assert.equal(
    selectRollingReviewTaskFromIdentities(
      [task],
      parseRollingReviewResultFileNames([`${REVIEW_RESULT_PREFIX}${fileName}`]),
    ),
    null,
  );
});

Deno.test("rolling review Gitleaks rejection keeps unreachable semantics only for unanchored targets", () => {
  const task = push(932);
  const observedAt = "2026-08-31T00:00:00.000Z";
  const scanFailure = new Error(GITLEAKS_TARGET_REJECTION);
  // A genuinely unanchored target has no distinct base to preserve, so the
  // existing unreachable disposition is retained: base_sha null, exact safe
  // failure, zero findings, never completed, never ingested, never reselected.
  const deferred = deferGitleaksRejectedTarget(
    scanFailure,
    { prNumber: task.number, prUrl: task.htmlUrl, headSha: task.headSha, headRef: task.headRef },
    null,
    observedAt,
  );
  const fileName = rollingReviewResultFileName(task.number, task.headSha);
  assert.equal(deferred.status, "unreachable");
  assert.equal(deferred.parse_status, "unreachable");
  assert.equal(deferred.base_sha, null);
  assert.equal(deferred.failure, GITLEAKS_TARGET_REJECTION);
  assert.deepEqual(deferred.findings, []);
  assert.equal(
    parseRollingReviewResult(deferred, { prNumber: task.number, headSha: task.headSha, fileName }).base_sha,
    null,
  );
  const outcome = analyzeRollingReviewRecord(deferred);
  assert.deepEqual(outcome.priorityFindings, []);
  assert.deepEqual(outcome.backlogFindings, []);
  assert.equal(outcome.report, null);
  assert.equal(
    applyRollingReviewIngestion(renderReviewBacklog([]), [outcome], new Date("2026-08-31T00:00:01.000Z")),
    renderReviewBacklog([]),
  );
  assert.equal(selectNextRollingReviewTask([task], [deferred]), null);
});

Deno.test("rolling review Gitleaks rejection fail-closes on invalid base and only the exact rejection", () => {
  const task = push(933);
  const observedAt = "2026-08-31T00:00:00.000Z";
  const target = { prNumber: task.number, prUrl: task.htmlUrl, headSha: task.headSha, headRef: task.headRef };
  // An anchored rejection must carry a well-formed exact base distinct from
  // the head: an invalid base fails closed through the strict contract
  // instead of manufacturing evidence.
  assert.throws(
    () => deferGitleaksRejectedTarget(new Error(GITLEAKS_TARGET_REJECTION), target, task.headSha, observedAt),
    /base SHA is invalid/u,
  );
  assert.throws(
    () => deferGitleaksRejectedTarget(new Error(GITLEAKS_TARGET_REJECTION), target, "not-a-sha", observedAt),
    /base SHA is invalid/u,
  );
  // Only the exact rejection is deferred: every other scan or tooling error
  // still throws through the worker unchanged instead of being masked.
  assert.throws(
    () => deferGitleaksRejectedTarget(new Error("unexpected scan failure"), target, "b".repeat(40), observedAt),
    /unexpected scan failure/u,
  );
  assert.throws(
    () =>
      deferGitleaksRejectedTarget(
        new TypeError(GITLEAKS_TARGET_REJECTION),
        target,
        "b".repeat(40),
        observedAt,
      ),
    /Gitleaks rejected the candidate/u,
  );
  assert.throws(
    () => deferGitleaksRejectedTarget(GITLEAKS_TARGET_REJECTION, target, "b".repeat(40), observedAt),
    /Gitleaks rejected the candidate/u,
  );
});

Deno.test("the checked-in review backlog strict-parses every complete table row", () => {
  // The backlog is a live growing queue: the expectation is derived from the
  // imported document itself, never frozen to the current row set.
  const tableRowCount = backlogMarkdown.split("\n").filter((line) => {
    if (!line.startsWith("|")) return false;
    const cells = line.slice(1, -1).split("|");
    const first = cells[0]?.trim() ?? "";
    return cells.length === 8 && first !== "Fingerprint" && !/^-+$/u.test(first);
  }).length;
  const entries = parseReviewBacklog(backlogMarkdown);
  assert.ok(entries.length > 0, "the checked-in review backlog must not be empty");
  assert.equal(entries.length, tableRowCount, "strict parse must accept every complete table row");
  // Canonical roundtrip: the strict parser already requires the exact canonical
  // render; re-rendering and re-parsing must reproduce the same entries.
  assert.equal(renderReviewBacklog(entries), backlogMarkdown);
  assert.deepEqual(parseReviewBacklog(renderReviewBacklog(entries)), entries);
});
