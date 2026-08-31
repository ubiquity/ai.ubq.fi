import { CodexInvocationError, runNativeCodexReview } from "./codex.ts";
import { SENTINEL_POLICY } from "./policy.ts";
import {
  analyzeRollingReviewRecord,
  applyRollingReviewIngestion,
  boundedRawReview,
  isSentinelRollingReviewPull,
  parseCompletedRollingReview,
  parseRollingReviewResult,
  parseRollingReviewResultFileName,
  rejectedRollingReviewRecord,
  resolveRollingReviewTargetAnchor,
  rollingReviewRequestId,
  type RollingReviewResult,
  rollingReviewResultFileName,
  selectNextRollingReviewTask,
  type SentinelPullRequest,
  unreachableRollingReviewRecord,
} from "./rolling-review.ts";
import {
  assertGitHistoryExcludesValues,
  runTrustedGit,
  runTrustedGitUnchecked,
  scanCandidateWithGitleaks,
} from "./validation.ts";

/**
 * Asynchronous rolling Codex review worker.
 *
 * This entrypoint is the only component that executes a Codex review after a
 * Sentinel pull request was already delivered. It never gates delivery,
 * merging, or testing: when no review is due or the review invocation cannot
 * run, it records an `unavailable` outcome and exits cleanly. A review target
 * that cannot be anchored to development (git merge-base finds no common
 * ancestor, or the merge base is the head itself) also exits cleanly after
 * retaining a durable `unreachable` disposition, so a historical or displaced
 * candidate never blocks Sentinel forward progress. A review target whose
 * candidate working tree or Git history is rejected by the Gitleaks gate is
 * retained with a durable non-complete disposition carrying the exact safe
 * rejection reason: an anchored target keeps the exact proven base SHA in the
 * fail-closed `rejected` disposition, while a genuinely unanchored target
 * keeps its existing `unreachable` semantics. Its review evidence is never
 * published and zero findings are ingested, while the worker still exits
 * cleanly. Every durable review result is strictly validated before
 * ingestion, and any malformed or identity-mismatched data fails closed
 * (nothing ingested, nothing pushed, exact evidence retained, non-zero exit).
 */

/**
 * The exact target-validation rejection thrown by `scanCandidateWithGitleaks`
 * when Gitleaks flags the candidate working tree or Git history. The message
 * is fixed and contains no secret material, so it is safe to persist verbatim
 * as the exact rejection reason of the durable disposition.
 */
export const GITLEAKS_TARGET_REJECTION = "Gitleaks rejected the candidate or Git history";

/**
 * Fail-closed handling of one Gitleaks target-validation rejection: only the
 * exact rejection above (a plain `Error` carrying the exact message thrown by
 * `scanCandidateWithGitleaks`) converts the selected target into a durable
 * non-complete disposition carrying the same exact PR/head identity and the
 * exact safe rejection reason. An anchored target (`baseSha` is its proven
 * exact merge base) becomes the strict `rejected` disposition that preserves
 * that exact base SHA; a genuinely unanchored target (`baseSha` is null)
 * keeps the existing `unreachable` semantics with a null base. Any other
 * error is an unexpected scan or tooling failure and rethrows unchanged, so
 * the worker still fails instead of masking it.
 */
export const deferGitleaksRejectedTarget = (
  error: unknown,
  target: Readonly<{ prNumber: number; prUrl: string; headSha: string; headRef: string }>,
  baseSha: string | null,
  observedAt: string,
): RollingReviewResult => {
  if (!(error instanceof Error) || error.constructor !== Error || error.message !== GITLEAKS_TARGET_REJECTION) {
    throw error;
  }
  if (baseSha === null) {
    return unreachableRollingReviewRecord({
      prNumber: target.prNumber,
      prUrl: target.prUrl,
      headSha: target.headSha,
      headRef: target.headRef,
      failure: error.message,
      observedAt,
    });
  }
  return rejectedRollingReviewRecord({
    prNumber: target.prNumber,
    prUrl: target.prUrl,
    headSha: target.headSha,
    baseSha,
    headRef: target.headRef,
    failure: error.message,
    observedAt,
  });
};

const API_VERSION = "2022-11-28";
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const MAX_PULL_PAGES = 10;
const MAX_REVIEW_RESULTS = 256;
const MAX_REVIEW_RESULT_FILE_BYTES = 512 * 1_024;
const MAX_REVIEW_BACKLOG_BYTES_ON_READ = 512 * 1_024;
const ROLLING_REVIEW_INVOCATION_MS = 40 * 60 * 1_000;

const requiredEnvironment = (name: string): string => {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`${name} is required for the Sentinel rolling review worker`);
  return value;
};

const optionalEnvironment = (name: string): string | undefined => Deno.env.get(name)?.trim() || undefined;

const decodeUtf8 = (stdout: Uint8Array): string => new TextDecoder("utf-8", { fatal: true }).decode(stdout);

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const gitEnvironment = (token: string): Readonly<Record<string, string>> => ({
  GIT_TERMINAL_PROMPT: "0",
  GIT_CONFIG_COUNT: "1",
  GIT_CONFIG_KEY_0: "http.https://github.com/.extraheader",
  GIT_CONFIG_VALUE_0: `AUTHORIZATION: basic ${btoa(`x-access-token:${token}`)}`,
});

const writeJsonFile = async (path: string, value: unknown): Promise<void> => {
  await Deno.writeTextFile(path, `${JSON.stringify(value, null, 2)}\n`, { mode: 0o600 });
};

const githubRequest = async (token: string, repository: string, path: string): Promise<unknown> => {
  const response = await fetch(`https://api.github.com/repos/${repository}${path}`, {
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": API_VERSION,
    },
    signal: AbortSignal.timeout(30_000),
  });
  const text = await response.text();
  if (!response.ok) {
    throw new Error(`GitHub API GET ${path} failed with HTTP ${response.status}: ${text.slice(0, 500)}`);
  }
  return JSON.parse(text);
};

/**
 * Parses one page of the GitHub REST pull-request listing into the minimal
 * pull-request identity the rolling scan keys on. The live GitHub pull payload
 * reports an absent description as `body: null`, which is a valid contract
 * value (the shared GitHub parsers normalize it to `""`); every other field
 * must carry the exact PR/head/base identity the rolling review requires. Any
 * truly malformed record fails closed before the page can be used: no
 * fallback, no partial parse, no guessed identity.
 */
export const parseSentinelPullList = (value: unknown): SentinelPullRequest[] => {
  if (!Array.isArray(value)) throw new Error("GitHub pull-request listing is invalid");
  const pulls: SentinelPullRequest[] = [];
  for (const candidate of value) {
    if (!isRecord(candidate)) throw new Error("GitHub returned an invalid pull request");
    const head = isRecord(candidate.head) ? candidate.head : null;
    const base = isRecord(candidate.base) ? candidate.base : null;
    if (
      !Number.isSafeInteger(candidate.number) || (candidate.number as number) <= 0 ||
      typeof candidate.html_url !== "string" || !/^https:\/\/github\.com\//u.test(candidate.html_url) ||
      (candidate.state !== "open" && candidate.state !== "closed") ||
      !(candidate.merged_at === null || typeof candidate.merged_at === "string") ||
      !(candidate.body === null || typeof candidate.body === "string") || !head || !base ||
      typeof head.ref !== "string" || !SAFE_BRANCH.test(head.ref) ||
      typeof head.sha !== "string" || !FULL_SHA.test(head.sha) ||
      typeof base.ref !== "string" || !SAFE_BRANCH.test(base.ref)
    ) {
      throw new Error("GitHub returned an invalid pull request");
    }
    pulls.push({
      number: candidate.number as number,
      htmlUrl: candidate.html_url as string,
      state: candidate.state as "open" | "closed",
      merged: candidate.merged_at !== null,
      headRef: head.ref,
      headSha: head.sha,
      baseRef: base.ref,
      body: candidate.body ?? "",
    });
  }
  return pulls;
};

const listSentinelPulls = async (token: string, repository: string): Promise<SentinelPullRequest[]> => {
  const pulls: SentinelPullRequest[] = [];
  for (let page = 1; page <= MAX_PULL_PAGES; page++) {
    const value = await githubRequest(token, repository, `/pulls?state=all&per_page=100&page=${page}`);
    const pagePulls = parseSentinelPullList(value);
    pulls.push(...pagePulls);
    if (pagePulls.length < 100) break;
  }
  return pulls;
};

const listDevelopmentReviewResults = async (
  root: string,
  gitEnv: Readonly<Record<string, string>>,
): Promise<RollingReviewResult[]> => {
  const tree = await runTrustedGit({
    args: ["ls-tree", "-r", "--name-only", "origin/development", "--", SENTINEL_POLICY.paths.reviewResults],
    cwd: root,
    env: gitEnv,
    maximumOutputBytes: 512 * 1_024,
  });
  const names = new TextDecoder("utf-8").decode(tree.stdout)
    .split("\n")
    .filter((line) => line.length > 0)
    .filter((line) => line.startsWith(`${SENTINEL_POLICY.paths.reviewResults}/`))
    .map((line) => line.slice(SENTINEL_POLICY.paths.reviewResults.length + 1));
  if (names.length > MAX_REVIEW_RESULTS) {
    throw new Error("Sentinel rolling review result directory exceeds its entry limit");
  }
  const results: RollingReviewResult[] = [];
  for (const name of names) {
    const identity = parseRollingReviewResultFileName(name);
    if (identity === null) {
      throw new Error(`Rolling review result file name is invalid: ${name}`);
    }
    const blob = await runTrustedGit({
      args: ["show", `origin/development:${SENTINEL_POLICY.paths.reviewResults}/${name}`],
      cwd: root,
      env: gitEnv,
      maximumOutputBytes: MAX_REVIEW_RESULT_FILE_BYTES,
    });
    if (blob.stdout.byteLength > MAX_REVIEW_RESULT_FILE_BYTES) {
      throw new Error(`Rolling review result file exceeds its byte limit: ${name}`);
    }
    let value: unknown;
    try {
      value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(blob.stdout));
    } catch (error) {
      throw new Error(
        `Rolling review result file is not valid JSON (${name}): ${
          error instanceof Error ? error.message : "parse failed"
        }`,
      );
    }
    results.push(
      parseRollingReviewResult(value, { prNumber: identity.prNumber, headSha: identity.headSha, fileName: name }),
    );
  }
  return results;
};

const readDevelopmentBacklog = async (
  root: string,
  gitEnv: Readonly<Record<string, string>>,
): Promise<string> => {
  const blob = await runTrustedGit({
    args: ["show", `origin/development:${SENTINEL_POLICY.paths.reviewBacklog}`],
    cwd: root,
    env: gitEnv,
    maximumOutputBytes: MAX_REVIEW_BACKLOG_BYTES_ON_READ,
  });
  return new TextDecoder("utf-8", { fatal: true }).decode(blob.stdout);
};

type AuthSlots = Readonly<{ slot1B64?: string; slot2B64?: string }>;

const authSlotsFromPrivateState = async (): Promise<AuthSlots> => {
  const stateDirectory = requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_DIR");
  const runnerTemp = requiredEnvironment("RUNNER_TEMP");
  const expectedDirectory = `${runnerTemp}/sentinel-codex-auth-state`;
  if (!runnerTemp.startsWith("/") || stateDirectory !== expectedDirectory) {
    throw new Error("Sentinel Codex auth state directory is not the expected private runner path");
  }
  const readSlot = async (slot: 1 | 2): Promise<string | undefined> => {
    const path = `${stateDirectory}/slots/${slot}/auth.json`;
    try {
      const bytes = await Deno.readFile(path);
      return btoa(String.fromCharCode(...bytes));
    } catch (error) {
      if (error instanceof Deno.errors.NotFound) return undefined;
      throw error;
    }
  };
  const [slot1B64, slot2B64] = await Promise.all([readSlot(1), readSlot(2)]);
  return { slot1B64, slot2B64 };
};

const currentDevelopmentSha = async (
  root: string,
  gitEnv: Readonly<Record<string, string>>,
): Promise<string> => {
  await runTrustedGit({ args: ["fetch", "--no-tags", "origin", "development"], cwd: root, env: gitEnv });
  const value = decodeUtf8(
    (await runTrustedGit({ args: ["rev-parse", "origin/development"], cwd: root, env: gitEnv })).stdout,
  ).trim();
  if (!FULL_SHA.test(value)) throw new Error("Running review worker could not resolve origin/development");
  return value;
};

const run = async (): Promise<void> => {
  const root = await Deno.realPath(requiredEnvironment("GITHUB_WORKSPACE"));
  const token = requiredEnvironment("GITHUB_TOKEN");
  const repository = requiredEnvironment("GITHUB_REPOSITORY");
  if (!SAFE_REPOSITORY.test(repository)) throw new Error("GITHUB_REPOSITORY is invalid");
  const mode = Deno.env.get("SENTINEL_MODE")?.trim();
  const gitEnv = gitEnvironment(token);
  const runnerTemp = requiredEnvironment("RUNNER_TEMP");
  const reportsDir = `${root}/${SENTINEL_POLICY.paths.reports}`;
  await Deno.mkdir(reportsDir, { recursive: true, mode: 0o700 });
  await Deno.mkdir(`${runnerTemp}/rolling-review`, { recursive: true, mode: 0o700 });
  const reportPath = `${reportsDir}/rolling-review-worker.json`;

  if (mode !== "hourly") {
    await writeJsonFile(reportPath, {
      schema_version: 1,
      status: "not_applicable",
      reason: "Rolling Codex reviews run only for the hourly cycle.",
    });
    return;
  }

  await currentDevelopmentSha(root, gitEnv);
  const pulls = await listSentinelPulls(token, repository);
  const existing = await listDevelopmentReviewResults(root, gitEnv);
  const task = selectNextRollingReviewTask(pulls, existing);
  if (task === null) {
    await writeJsonFile(reportPath, {
      schema_version: 1,
      status: "no_pending",
      eligible_pulls: pulls.filter(isSentinelRollingReviewPull).length,
      pending_count: 0,
    });
    return;
  }

  const reviewStart = Date.now();
  const authSlots = await authSlotsFromPrivateState();
  if (!authSlots.slot1B64 && !authSlots.slot2B64) {
    throw new Error("At least one Sentinel Codex auth slot is required for the rolling review worker");
  }
  const scratch = `${runnerTemp}/rolling-review/scratch`;
  let reviewFailure: string | null = null;
  let targetFailure: string | null = null;
  let rawReview = "";
  let reviewStderr = "";
  let structuredReview: unknown | null = null;
  let mergeBase = "";
  let record: RollingReviewResult;
  try {
    // A merged Sentinel pull request may have had its candidate branch
    // deleted; the exact head commit is still reachable from development, so
    // prefer the locally reachable commit and fall back to the head branch
    // for an open pull request.
    let fetchedHead = "";
    const locallyPresent = await runTrustedGitUnchecked({
      args: ["cat-file", "-e", `${task.headSha}^{commit}`],
      cwd: root,
      env: gitEnv,
    });
    if (locallyPresent.code === 0) {
      fetchedHead = task.headSha;
    } else {
      const branchFetch = await runTrustedGitUnchecked({
        args: [
          "fetch",
          "--no-tags",
          "origin",
          `+refs/heads/${task.headRef}:refs/remotes/origin/${task.headRef}`,
        ],
        cwd: root,
        env: gitEnv,
      });
      if (branchFetch.code !== 0) {
        await runTrustedGit({
          args: ["fetch", "--no-tags", "origin", task.headSha],
          cwd: root,
          env: gitEnv,
        });
        fetchedHead = decodeUtf8(
          (await runTrustedGit({ args: ["rev-parse", "FETCH_HEAD"], cwd: root, env: gitEnv })).stdout,
        ).trim();
      } else {
        fetchedHead = decodeUtf8(
          (await runTrustedGit({
            args: ["rev-parse", `refs/remotes/origin/${task.headRef}^{commit}`],
            cwd: root,
            env: gitEnv,
          })).stdout,
        ).trim();
      }
    }
    if (fetchedHead !== task.headSha) {
      throw new Error(`Rolling review pull request head changed: ${task.headRef}`);
    }
    await runTrustedGit({
      args: ["fetch", "--no-tags", "origin", "development"],
      cwd: root,
      env: gitEnv,
    });
    // The merge-base result is fail-closed: `git merge-base` exits 1 without
    // any stderr when the head has no common ancestor with development, and a
    // merge base equal to the head means the reviewed head is already part of
    // development. Neither is a review failure to retry — both are durable
    // unreachable dispositions recorded for the exact identity so Sentinel
    // continues instead of re-selecting the same target forever.
    const anchorResult = await runTrustedGitUnchecked({
      args: ["merge-base", task.headSha, "origin/development"],
      cwd: root,
      env: gitEnv,
    });
    const anchor = resolveRollingReviewTargetAnchor(
      {
        code: anchorResult.code,
        stdout: new TextDecoder("utf-8").decode(anchorResult.stdout),
        stderr: new TextDecoder("utf-8").decode(anchorResult.stderr),
      },
      task.headSha,
    );
    if (anchor.status === "unreachable") {
      targetFailure = anchor.failure;
    } else {
      mergeBase = anchor.baseSha;
      await runTrustedGit({
        args: ["worktree", "add", "--detach", "--force", scratch, task.headSha],
        cwd: root,
        env: gitEnv,
      });
      await runTrustedGit({
        args: ["update-ref", "refs/remotes/origin/development", mergeBase],
        cwd: scratch,
        env: gitEnv,
      });
      try {
        const review = await runNativeCodexReview({
          checkoutPath: scratch,
          authSlots,
          expectedMaximumRuntimeMs: ROLLING_REVIEW_INVOCATION_MS,
        });
        const bounded = boundedRawReview(review.stdout, review.stderr);
        rawReview = bounded.stdout;
        reviewStderr = bounded.stderr;
        structuredReview = review.nativeReviewOutput ?? null;
      } catch (error) {
        if (error instanceof CodexInvocationError && error.failure === "secret_in_output") throw error;
        reviewFailure = error instanceof Error ? error.message : "Rolling review invocation failed";
      }
    }
  } finally {
    await runTrustedGitUnchecked({
      args: ["worktree", "remove", "--force", scratch],
      cwd: root,
      env: gitEnv,
    }).catch(() => undefined);
  }
  if (targetFailure !== null) {
    record = unreachableRollingReviewRecord({
      prNumber: task.number,
      prUrl: task.htmlUrl,
      headSha: task.headSha,
      headRef: task.headRef,
      failure: targetFailure,
      observedAt: new Date(reviewStart).toISOString(),
    });
  } else if (reviewFailure !== null) {
    await writeJsonFile(reportPath, {
      schema_version: 1,
      status: "unavailable",
      pr_number: task.number,
      head_sha: task.headSha,
      head_branch: task.headRef,
      failure: reviewFailure.slice(0, 2_000),
    });
    return;
  } else {
    const parsed = await parseCompletedRollingReview({
      rawReviewText: rawReview,
      reviewStderr,
      structuredReview,
      checkoutPath: scratch,
      round: 1,
    });
    record = {
      schema_version: 1,
      request_id: rollingReviewRequestId(task.number, task.headSha),
      pr_number: task.number,
      pr_url: task.htmlUrl,
      head_sha: task.headSha,
      base_sha: mergeBase,
      head_branch: task.headRef,
      status: parsed.parse_status === "unparseable" ? "unparseable" : "completed",
      reviewed_at: new Date(reviewStart).toISOString(),
      parse_status: parsed.parse_status,
      raw_review_text: rawReview,
      review_stderr: reviewStderr,
      structured_review: structuredReview,
      findings: parsed.parse_status === "unparseable" ? [] : parsed.report.findings,
      failure: parsed.failure,
    };
  }
  const fileStem = rollingReviewResultFileName(record.pr_number, record.head_sha);
  const resultPath = `${SENTINEL_POLICY.paths.reviewResults}/${fileStem}`;
  // Fail-closed self-check before durable retention: the record must satisfy
  // exactly the strict identity, consistency, and size validation that later
  // cycles apply when they ingest it.
  parseRollingReviewResult(record, { prNumber: record.pr_number, headSha: record.head_sha, fileName: fileStem });

  // Evidence and backlog ingestion are committed from a separate docs-only
  // worktree so the primary checkout stays untouched: one-parent commit on
  // the exact development tip, gitleaks and secret-history gates, then push.
  // Refresh development first: the review scratch rewrote the shared
  // origin/development ref to the reviewed merge base.
  await currentDevelopmentSha(root, gitEnv);
  const docsScratch = `${runnerTemp}/rolling-review/docs`;
  await runTrustedGit({
    args: ["worktree", "add", "--detach", "--force", docsScratch, "origin/development"],
    cwd: root,
    env: gitEnv,
  });
  try {
    const currentBacklog = await readDevelopmentBacklog(docsScratch, gitEnv);
    const resultDirectory = `${docsScratch}/${SENTINEL_POLICY.paths.reviewResults}`;
    await Deno.mkdir(resultDirectory, { recursive: true, mode: 0o700 });
    await writeJsonFile(`${resultDirectory}/${fileStem}`, record);
    try {
      await scanCandidateWithGitleaks({
        cwd: docsScratch,
        reportPath: `${reportsDir}/secret-scan-rolling-review.json`,
      });
    } catch (error) {
      // The candidate working tree or Git history was rejected by Gitleaks, so
      // this target's review evidence cannot be published safely. An anchored
      // target keeps the exact proven merge base in the strict `rejected`
      // disposition (never base_sha:null): the exact reviewed head and base
      // identity survive with zero findings and the exact safe rejection
      // reason. A genuinely unanchored target keeps its existing unreachable
      // semantics. Any other error rethrows unchanged.
      record = deferGitleaksRejectedTarget(
        error,
        { prNumber: task.number, prUrl: task.htmlUrl, headSha: task.headSha, headRef: task.headRef },
        record.base_sha,
        new Date(reviewStart).toISOString(),
      );
      await writeJsonFile(`${resultDirectory}/${fileStem}`, record);
    }
    const outcomes = [analyzeRollingReviewRecord(record)];
    const nextBacklog = applyRollingReviewIngestion(currentBacklog, outcomes, new Date(reviewStart));
    await assertGitHistoryExcludesValues({
      cwd: docsScratch,
      sensitiveValues: [
        token,
        ...(authSlots.slot1B64 !== undefined ? [authSlots.slot1B64] : []),
        ...(authSlots.slot2B64 !== undefined ? [authSlots.slot2B64] : []),
      ],
    });
    const before = decodeUtf8(
      (await runTrustedGit({ args: ["rev-parse", "HEAD"], cwd: docsScratch, env: gitEnv })).stdout,
    ).trim();
    await runTrustedGit({
      args: ["add", "--", `${SENTINEL_POLICY.paths.reviewResults}/`, SENTINEL_POLICY.paths.reviewBacklog],
      cwd: docsScratch,
      env: gitEnv,
    });
    const changedPaths = decodeUtf8(
      (await runTrustedGit({
        args: ["diff", "--cached", "--name-only", "-z"],
        cwd: docsScratch,
        env: gitEnv,
      })).stdout,
    ).split("\0").filter((path) => path.length > 0).sort();
    const allowed = [resultPath, SENTINEL_POLICY.paths.reviewBacklog];
    if (
      changedPaths.length > 2 || changedPaths.some((path) => !allowed.includes(path)) ||
      (nextBacklog === currentBacklog && changedPaths.length !== 1) ||
      (nextBacklog !== currentBacklog && changedPaths.length !== 2)
    ) {
      throw new Error("Rolling review docs commit changed an unexpected path set");
    }
    const commitMessage = record.status === "unreachable"
      ? `docs: record unreachable Sentinel rolling review target for PR #${task.number}`
      : record.status === "rejected"
      ? `docs: record Gitleaks-rejected Sentinel rolling review target for PR #${task.number}`
      : `docs: ingest completed Sentinel rolling Codex review for PR #${task.number}`;
    await runTrustedGit({
      args: ["commit", "-m", commitMessage],
      cwd: docsScratch,
      env: gitEnv,
    });
    const tip = await currentDevelopmentSha(root, gitEnv);
    if (before !== tip) {
      throw new Error("origin/development advanced while the rolling review evidence was prepared");
    }
    await runTrustedGit({
      args: ["push", "origin", "HEAD:refs/heads/development"],
      cwd: docsScratch,
      env: gitEnv,
    });
    const pushed = await currentDevelopmentSha(root, gitEnv);
    const githubEnvironment = optionalEnvironment("GITHUB_ENV");
    if (githubEnvironment) {
      await Deno.writeTextFile(githubEnvironment, `SENTINEL_BACKLOG_HINT_SHA=${pushed}\n`, { append: true });
    }
    await writeJsonFile(reportPath, {
      schema_version: 1,
      status: record.status,
      pr_number: task.number,
      head_sha: task.headSha,
      head_branch: task.headRef,
      request_id: record.request_id,
      parse_status: record.parse_status,
      findings_count: record.findings.length,
      priority_findings_count: outcomes[0]!.priorityFindings.length,
      reviewed_at: record.reviewed_at,
      failure: record.failure,
      pushed_development_sha: pushed,
    });
    if (record.status === "unparseable") {
      throw new Error(
        `Rolling Codex review for PR #${task.number} at ${task.headSha} was not parseable; exact evidence is ` +
          `retained at ${resultPath}, nothing was ingested, and the review is not retried automatically`,
      );
    }
  } finally {
    await runTrustedGitUnchecked({
      args: ["worktree", "remove", "--force", docsScratch],
      cwd: root,
      env: gitEnv,
    }).catch(() => undefined);
  }
};

if (import.meta.main) {
  try {
    await run();
  } catch (error) {
    console.error("[sentinel] rolling_review_worker_failed", error instanceof Error ? error.message : error);
    throw error;
  }
}
