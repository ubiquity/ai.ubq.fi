import assert from "node:assert/strict";
import type { GitHubActionsClient, GitHubArtifact } from "../scripts/sentinel/github.ts";
import {
  aggregateCandidateChangedPaths,
  assertGitHubIssueManualCheckpointCodeTreeEquivalent,
  captureFailedCandidateSnapshot,
  loadMatchingRetainedCaptures,
  prepareImmutableTemporaryCheckpoint,
  prepareResumedGitHubIssueCandidate,
  pushRetryPendingRefsAtomically,
  replayIndexArtifactName,
  requireResolvedReviewBacklogImplementation,
  restoreIssueRetryAggregateIfEmpty,
  RetryCheckpointResumeError,
  retryCheckpointResumeFailureDisposition,
  reviewBacklogAffectedPathChangedAtSelectedBase,
  writeReplayArtifactMetadata,
} from "../scripts/sentinel/main.ts";
import { sentinelRecoveryCandidateBranch } from "../scripts/sentinel/recovery.ts";
import {
  applyGitHubIssueJobDisposition,
  type GitHubIssueJob,
  parseGitHubIssueJobLedger,
  renderGitHubIssueJobLedger,
} from "../scripts/sentinel/issues.ts";
import { captureRawDenoLogs, persistCandidateValidationFailure } from "../scripts/sentinel/validation.ts";
import { type ExportedSentinelReplayCapture, SENTINEL_REPLAY_TTL_MS } from "../src/sentinel_replay_capture.ts";

const requiredPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);

const encodeBase64Url = (bytes: Uint8Array): string =>
  btoa(String.fromCharCode(...bytes)).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

Deno.test({
  name: "candidate validation preserves exact failed output as private sidecars",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const directory = await Deno.makeTempDir({ prefix: "sentinel-validation-failure-" });
    const stdout = new Uint8Array([0, 1, 254, 255]);
    const stderr = new TextEncoder().encode("fixture failure\n");
    try {
      const failure = await persistCandidateValidationFailure({
        reportPath: `${directory}/validation-round-1.json`,
        phase: "repository_tests",
        command: ["deno", "test", "--cached-only"],
        exitCode: 1,
        durationMs: 42,
        stdout,
        stderr,
      });
      assert.deepEqual(await Deno.readFile(failure.stdout_path), stdout);
      assert.deepEqual(await Deno.readFile(failure.stderr_path), stderr);
      assert.equal(failure.stdout_bytes, stdout.byteLength);
      assert.equal(failure.stderr_bytes, stderr.byteLength);
      assert.match(failure.stdout_sha256, /^[0-9a-f]{64}$/u);
      assert.match(failure.stderr_sha256, /^[0-9a-f]{64}$/u);
      assert.equal(failure.stdout_truncated, false);
      assert.equal(failure.stderr_excerpt, "fixture failure\n");
      assert.equal((await Deno.stat(failure.stdout_path)).mode! & 0o077, 0);
      assert.equal((await Deno.stat(failure.stderr_path)).mode! & 0o077, 0);
    } finally {
      stdout.fill(0);
      stderr.fill(0);
      await Deno.remove(directory, { recursive: true });
    }
  },
});

const capture = (
  captureId: string,
  fingerprintCharacter: string,
  groupCharacter: string,
  capturedAtMs: number,
): ExportedSentinelReplayCapture => ({
  manifest: {
    version: 1,
    capture_id: captureId,
    fingerprint: fingerprintCharacter.repeat(64),
    case_group_digest: groupCharacter.repeat(64),
    captured_at_ms: capturedAtMs,
    expires_at_ms: capturedAtMs + SENTINEL_REPLAY_TTL_MS,
    algorithm: "AES-256-GCM",
    compression: "gzip",
    iv: encodeBase64Url(new Uint8Array(12)),
    chunk_count: 1,
    ciphertext_bytes: 16,
  },
  chunks: [encodeBase64Url(new Uint8Array(16))],
});

Deno.test({
  name: "aggregate backlog binding rejects a repair that restores the base code",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-aggregate-candidate-" });
    const checkout = `${root}/checkout`;
    const git = async (args: string[]): Promise<string> => {
      const output = await new Deno.Command("git", {
        args,
        cwd: checkout,
        stdout: "piped",
        stderr: "piped",
      }).output();
      if (!output.success) throw new Error(new TextDecoder().decode(output.stderr));
      return new TextDecoder().decode(output.stdout).trim();
    };
    try {
      await Deno.mkdir(`${checkout}/src`, { recursive: true });
      await Deno.mkdir(`${checkout}/docs`, { recursive: true });
      await git(["init", "-b", "development"]);
      await Deno.writeTextFile(`${checkout}/src/handler.ts`, "export const behavior = 'base';\n");
      await Deno.writeTextFile(`${checkout}/docs/sentinel-review-backlog.md`, "open\n");
      await Deno.writeTextFile(`${checkout}/README.md`, "base\n");
      await git(["add", "src/handler.ts", "docs/sentinel-review-backlog.md", "README.md"]);
      await git([
        "-c",
        "user.name=Sentinel Test",
        "-c",
        "user.email=sentinel@example.invalid",
        "commit",
        "-m",
        "base",
      ]);
      const baseSha = await git(["rev-parse", "HEAD"]);

      await Deno.writeTextFile(`${checkout}/src/handler.ts`, "export const behavior = 'fixed';\n");
      await git(["add", "src/handler.ts"]);
      await git([
        "-c",
        "user.name=Sentinel Test",
        "-c",
        "user.email=sentinel@example.invalid",
        "commit",
        "-m",
        "candidate repair",
      ]);
      const repairSha = await git(["rev-parse", "HEAD"]);
      assert.equal(
        await reviewBacklogAffectedPathChangedAtSelectedBase(checkout, baseSha, repairSha, "src/handler.ts"),
        true,
      );
      assert.equal(
        await reviewBacklogAffectedPathChangedAtSelectedBase(checkout, baseSha, repairSha, "README.md"),
        false,
      );
      assert.equal(
        await reviewBacklogAffectedPathChangedAtSelectedBase(checkout, repairSha, baseSha, "src/handler.ts"),
        false,
      );
      await Deno.writeTextFile(`${checkout}/docs/sentinel-review-backlog.md`, "resolved\n");
      await git(["add", "docs/sentinel-review-backlog.md"]);
      await git([
        "-c",
        "user.name=Sentinel Test",
        "-c",
        "user.email=sentinel@example.invalid",
        "commit",
        "-m",
        "backlog disposition",
      ]);

      await Deno.writeTextFile(`${checkout}/src/handler.ts`, "export const behavior = 'base';\n");
      await Deno.writeTextFile(`${checkout}/README.md`, "unrelated repair residue\n");
      const aggregatePaths = [
        ...await aggregateCandidateChangedPaths(
          checkout,
          baseSha,
          ["docs/sentinel-review-backlog.md"],
        ),
      ].sort();
      assert.deepEqual(aggregatePaths, ["README.md"]);
      assert.throws(
        () =>
          requireResolvedReviewBacklogImplementation(
            "implemented",
            aggregatePaths,
            aggregatePaths,
            "src/handler.ts",
          ),
        /affected path/,
      );
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry checkpoints merge exact immutable work and fail closed on scope drift or conflict",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-retry-checkpoint-" });
    const remote = `${root}/remote.git`;
    const source = `${root}/source`;
    const checkout = `${root}/checkout`;
    const git = async (cwd: string, args: string[], allowFailure = false): Promise<string> => {
      const output = await new Deno.Command("git", {
        args,
        cwd,
        stdout: "piped",
        stderr: "piped",
      }).output();
      if (!output.success && !allowFailure) throw new Error(new TextDecoder().decode(output.stderr));
      return new TextDecoder().decode(output.stdout).trim();
    };
    try {
      await Deno.mkdir(source, { recursive: true });
      await git(root, ["init", "--bare", remote]);
      await git(source, ["init", "-b", "development"]);
      await git(source, ["config", "user.name", "Sentinel Test"]);
      await git(source, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.writeTextFile(`${source}/allowed.txt`, "base\n");
      await Deno.writeTextFile(`${source}/conflict.txt`, "base\n");
      await git(source, ["add", "allowed.txt", "conflict.txt"]);
      await git(source, ["commit", "-m", "base"]);
      const baseSha = await git(source, ["rev-parse", "HEAD"]);
      await git(source, ["remote", "add", "origin", remote]);
      await git(source, ["push", "origin", "development"]);

      await git(source, ["switch", "-c", "sentinel/candidate-101-1"]);
      await Deno.writeTextFile(`${source}/allowed.txt`, "checkpoint\n");
      await git(source, ["add", "allowed.txt"]);
      await git(source, ["commit", "-m", "safe checkpoint"]);
      const safeCheckpointSha = await git(source, ["rev-parse", "HEAD"]);
      await git(source, ["push", "origin", "sentinel/candidate-101-1"]);

      await git(source, ["switch", "-c", "sentinel/candidate-103", baseSha]);
      await Deno.writeTextFile(`${source}/conflict.txt`, "checkpoint conflict\n");
      await git(source, ["add", "conflict.txt"]);
      await git(source, ["commit", "-m", "conflicting checkpoint"]);
      const conflictCheckpointSha = await git(source, ["rev-parse", "HEAD"]);
      await git(source, ["push", "origin", "sentinel/candidate-103"]);

      await git(source, ["switch", "-c", "sentinel/candidate-105", baseSha]);
      await Deno.writeTextFile(`${source}/outside.txt`, "outside\n");
      await git(source, ["add", "outside.txt"]);
      await git(source, ["commit", "-m", "out of scope checkpoint"]);
      const outsideCheckpointSha = await git(source, ["rev-parse", "HEAD"]);
      await git(source, ["push", "origin", "sentinel/candidate-105"]);

      await git(source, ["switch", "development"]);
      await Deno.writeTextFile(`${source}/conflict.txt`, "development conflict\n");
      await Deno.writeTextFile(`${source}/development.txt`, "advanced\n");
      await git(source, ["add", "conflict.txt", "development.txt"]);
      await git(source, ["commit", "-m", "advance development"]);
      const developmentSha = await git(source, ["rev-parse", "HEAD"]);
      await git(source, ["push", "origin", "development"]);

      await git(root, ["clone", "--branch", "development", remote, checkout]);
      await git(checkout, ["config", "user.name", "Sentinel Test"]);
      await git(checkout, ["config", "user.email", "sentinel@example.invalid"]);
      await git(checkout, ["switch", "-c", "sentinel/candidate-101-2"]);
      const resumedSha = await prepareResumedGitHubIssueCandidate({
        checkout,
        candidateBranch: "sentinel/candidate-101-2",
        developmentSha,
        checkpoint: {
          branch: "sentinel/candidate-101-1",
          sha: safeCheckpointSha,
          baseSha,
        },
        allowedPaths: ["allowed.txt"],
        gitEnvironment: {},
      });
      assert.equal(await Deno.readTextFile(`${checkout}/allowed.txt`), "checkpoint\n");
      assert.equal(await git(checkout, ["merge-base", developmentSha, resumedSha]), developmentSha);
      assert.equal(await git(checkout, ["merge-base", safeCheckpointSha, resumedSha]), safeCheckpointSha);
      assert.equal(
        (await git(checkout, ["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-101-1"]))
          .split("\t")[0],
        safeCheckpointSha,
      );

      await Deno.writeTextFile(`${checkout}/allowed.txt`, "base\n");
      await git(checkout, ["add", "allowed.txt"]);
      assert.deepEqual(
        [...await aggregateCandidateChangedPaths(checkout, developmentSha)],
        [],
      );
      assert.deepEqual(
        await restoreIssueRetryAggregateIfEmpty(
          checkout,
          developmentSha,
          resumedSha,
          ["allowed.txt", "declared-but-absent.txt"],
        ),
        ["allowed.txt"],
      );
      assert.equal(await Deno.readTextFile(`${checkout}/allowed.txt`), "checkpoint\n");
      assert.equal(await git(checkout, ["status", "--porcelain=v1"]), "");
      assert.equal(
        await prepareImmutableTemporaryCheckpoint(
          checkout,
          "sentinel/candidate-101-2",
          resumedSha,
          {},
          null,
        ),
        resumedSha,
      );
      assert.equal(
        await git(checkout, ["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-101-2"]),
        "",
      );

      await git(checkout, ["switch", "-c", "sentinel/candidate-107", resumedSha]);
      await git(checkout, ["reset", "--hard", developmentSha]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "allowed residue after history rewrite\n");
      await assert.rejects(
        () =>
          restoreIssueRetryAggregateIfEmpty(
            checkout,
            developmentSha,
            resumedSha,
            ["allowed.txt"],
          ),
        /lost its pre-invocation commit/,
      );
      assert.equal(
        await git(checkout, ["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-107"]),
        "",
      );
      await git(checkout, ["restore", "--source", developmentSha, "--staged", "--worktree", "--", "allowed.txt"]);

      await git(checkout, ["switch", "development"]);
      await git(checkout, ["switch", "-c", "sentinel/candidate-102"]);
      assert.deepEqual(
        await restoreIssueRetryAggregateIfEmpty(
          checkout,
          developmentSha,
          developmentSha,
          ["allowed.txt"],
        ),
        [],
      );
      assert.equal(await git(checkout, ["status", "--porcelain=v1"]), "");

      await git(checkout, ["switch", "development"]);
      await git(checkout, ["switch", "-c", "sentinel/candidate-104"]);
      await assert.rejects(
        () =>
          prepareResumedGitHubIssueCandidate({
            checkout,
            candidateBranch: "sentinel/candidate-104",
            developmentSha,
            checkpoint: {
              branch: "sentinel/candidate-103",
              sha: conflictCheckpointSha,
              baseSha,
            },
            allowedPaths: ["conflict.txt"],
            gitEnvironment: {},
          }),
        (error) =>
          error instanceof RetryCheckpointResumeError &&
          retryCheckpointResumeFailureDisposition(error) === "manual_required" &&
          /conflicts with current development/u.test(error.message),
      );
      assert.equal(await git(checkout, ["rev-parse", "HEAD"]), developmentSha);
      assert.equal(await git(checkout, ["status", "--porcelain=v1"]), "");

      await git(checkout, ["switch", "development"]);
      await git(checkout, ["switch", "-c", "sentinel/candidate-106"]);
      await assert.rejects(
        () =>
          prepareResumedGitHubIssueCandidate({
            checkout,
            candidateBranch: "sentinel/candidate-106",
            developmentSha,
            checkpoint: {
              branch: "sentinel/candidate-105",
              sha: outsideCheckpointSha,
              baseSha,
            },
            allowedPaths: ["allowed.txt"],
            gitEnvironment: {},
          }),
        (error) =>
          error instanceof RetryCheckpointResumeError &&
          retryCheckpointResumeFailureDisposition(error) === "manual_required" &&
          /unsafe or out-of-scope path/u.test(error.message),
      );
      assert.equal(await git(checkout, ["rev-parse", "HEAD"]), developmentSha);
      assert.equal(await git(checkout, ["status", "--porcelain=v1"]), "");

      await git(checkout, ["switch", "development"]);
      await git(checkout, ["switch", "-c", "sentinel/candidate-108"]);
      await git(checkout, ["remote", "set-url", "origin", `${root}/unavailable.git`]);
      await assert.rejects(
        () =>
          prepareResumedGitHubIssueCandidate({
            checkout,
            candidateBranch: "sentinel/candidate-108",
            developmentSha,
            checkpoint: {
              branch: "sentinel/candidate-101-1",
              sha: safeCheckpointSha,
              baseSha,
            },
            allowedPaths: ["allowed.txt"],
            gitEnvironment: {},
          }),
        (error) =>
          error instanceof RetryCheckpointResumeError &&
          retryCheckpointResumeFailureDisposition(error) === "retry_pending" &&
          /remote lookup failed/u.test(error.message),
      );
      assert.equal(await git(checkout, ["rev-parse", "HEAD"]), developmentSha);
      assert.equal(await git(checkout, ["status", "--porcelain=v1"]), "");
      await git(checkout, ["remote", "set-url", "origin", remote]);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry checkpoint publication atomically updates development and its durable candidate ref",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-checkpoint-publish-" });
    const remote = `${root}/remote.git`;
    const checkout = `${root}/checkout`;
    const git = async (args: string[]): Promise<string> => {
      const output = await new Deno.Command("git", {
        args,
        cwd: checkout,
        stdout: "piped",
        stderr: "piped",
      }).output();
      if (!output.success) throw new Error(new TextDecoder().decode(output.stderr));
      return new TextDecoder().decode(output.stdout).trim();
    };
    try {
      await Deno.mkdir(checkout, { recursive: true });
      await new Deno.Command("git", { args: ["init", "--bare", remote] }).output();
      await git(["init", "-b", "development"]);
      await git(["config", "user.name", "Sentinel Test"]);
      await git(["config", "user.email", "sentinel@example.invalid"]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "base\n");
      await git(["add", "allowed.txt"]);
      await git(["commit", "-m", "base"]);
      const baseSha = await git(["rev-parse", "HEAD"]);
      await git(["remote", "add", "origin", remote]);
      await git(["push", "origin", "development"]);

      await git(["switch", "-c", "sentinel/candidate-201"]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "checkpoint\n");
      await git(["add", "allowed.txt"]);
      await git(["commit", "-m", "checkpoint"]);
      const checkpointSha = await git(["rev-parse", "HEAD"]);
      await git(["switch", "development"]);
      await Deno.writeTextFile(`${checkout}/ledger.txt`, "retry pending\n");
      await git(["add", "ledger.txt"]);
      await git(["commit", "-m", "record retry ledger"]);
      const dispositionSha = await git(["rev-parse", "HEAD"]);
      await pushRetryPendingRefsAtomically({
        checkout,
        developmentSha: dispositionSha,
        checkpoint: { branch: "sentinel/candidate-201", sha: checkpointSha, baseSha },
        expectedRemoteCheckpointSha: null,
        gitEnvironment: {},
      });
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-201"]))
          .split("\t")[0],
        checkpointSha,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/development"]))
          .split("\t")[0],
        dispositionSha,
      );

      await git(["switch", "-c", "sentinel/candidate-202"]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "second checkpoint\n");
      await git(["add", "allowed.txt"]);
      await git(["commit", "-m", "second checkpoint"]);
      const racedCheckpointSha = await git(["rev-parse", "HEAD"]);
      await git(["switch", "development"]);
      await Deno.writeTextFile(`${checkout}/ledger.txt`, "local retry pending\n");
      await git(["add", "ledger.txt"]);
      await git(["commit", "-m", "local retry ledger"]);
      const racedDispositionSha = await git(["rev-parse", "HEAD"]);

      const competing = `${root}/competing`;
      await new Deno.Command("git", { args: ["clone", "--branch", "development", remote, competing] }).output();
      const competingGit = async (args: string[]): Promise<string> => {
        const output = await new Deno.Command("git", {
          args,
          cwd: competing,
          stdout: "piped",
          stderr: "piped",
        }).output();
        if (!output.success) throw new Error(new TextDecoder().decode(output.stderr));
        return new TextDecoder().decode(output.stdout).trim();
      };
      await competingGit(["config", "user.name", "Sentinel Race Test"]);
      await competingGit(["config", "user.email", "sentinel-race@example.invalid"]);
      await Deno.writeTextFile(`${competing}/remote.txt`, "development advanced\n");
      await competingGit(["add", "remote.txt"]);
      await competingGit(["commit", "-m", "advance development"]);
      const advancedDevelopmentSha = await competingGit(["rev-parse", "HEAD"]);
      await competingGit(["push", "origin", "HEAD:refs/heads/development"]);
      await assert.rejects(
        () =>
          pushRetryPendingRefsAtomically({
            checkout,
            developmentSha: racedDispositionSha,
            checkpoint: { branch: "sentinel/candidate-202", sha: racedCheckpointSha, baseSha: dispositionSha },
            expectedRemoteCheckpointSha: null,
            gitEnvironment: {},
          }),
        /atomic push failed|git failed with/u,
      );
      assert.equal(
        await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-202"]),
        "",
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/development"])).split("\t")[0],
        advancedDevelopmentSha,
      );

      await git(["fetch", "origin", "development"]);
      await git(["reset", "--hard", "origin/development"]);
      await git(["switch", "-c", "sentinel/candidate-203"]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "third checkpoint\n");
      await git(["add", "allowed.txt"]);
      await git(["commit", "-m", "third checkpoint"]);
      const leasedCheckpointSha = await git(["rev-parse", "HEAD"]);
      await git(["push", "origin", `${advancedDevelopmentSha}:refs/heads/sentinel/candidate-203`]);
      await git(["switch", "development"]);
      await git(["reset", "--hard", "origin/development"]);
      await Deno.writeTextFile(`${checkout}/ledger.txt`, "lease mismatch retry\n");
      await git(["add", "ledger.txt"]);
      await git(["commit", "-m", "lease mismatch ledger"]);
      const leasedDispositionSha = await git(["rev-parse", "HEAD"]);
      await assert.rejects(
        () =>
          pushRetryPendingRefsAtomically({
            checkout,
            developmentSha: leasedDispositionSha,
            checkpoint: {
              branch: "sentinel/candidate-203",
              sha: leasedCheckpointSha,
              baseSha: advancedDevelopmentSha,
            },
            expectedRemoteCheckpointSha: null,
            gitEnvironment: {},
          }),
        /does not match the exact previously pushed SHA/u,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-203"]))
          .split("\t")[0],
        advancedDevelopmentSha,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/development"])).split("\t")[0],
        advancedDevelopmentSha,
      );
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "native-review exhaustion atomically retains an exact manual candidate or makes no false remote claim",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-manual-checkpoint-publish-" });
    const remote = `${root}/remote.git`;
    const checkout = `${root}/checkout`;
    const git = async (cwd: string, args: string[]): Promise<string> => {
      const output = await new Deno.Command("git", {
        args,
        cwd,
        stdout: "piped",
        stderr: "piped",
      }).output();
      if (!output.success) throw new Error(new TextDecoder().decode(output.stderr));
      return new TextDecoder().decode(output.stdout).trim();
    };
    const manualIssue = (number: number, fingerprintCharacter: string): GitHubIssueJob => ({
      repository: "ubiquity/ai.ubq.fi",
      issueId: 5_000_000_000 + number,
      nodeId: `I_kwDOQoe6nc8AAAABN6X${number}`,
      number,
      htmlUrl: `https://github.com/ubiquity/ai.ubq.fi/issues/${number}`,
      title: `Retain reviewed issue ${number}`,
      body: "Retain the exact declared candidate after review exhaustion.",
      bodySha256: "b".repeat(64),
      fingerprint: fingerprintCharacter.repeat(64),
      priority: "P2",
      priorityLabel: "Priority: 3 (High)",
      queuePriority: 3,
      queuePriorityAmbiguous: false,
      timeLabel: "Time: <1 Hour",
      intake: "owner_backlog",
      materialDigest: null,
      capturedComments: [],
      labels: ["Priority: 3 (High)", "Time: <1 Hour"],
      files: ["src/declared.ts", "src/declared-new.ts"],
      acceptance: ["The candidate remains available for manual review."],
      authorLogin: "0x4007",
      authorAssociation: "MEMBER",
      comments: 0,
      createdAt: "2026-08-28T00:00:00Z",
      updatedAt: "2026-08-28T00:00:00Z",
      relations: {
        parentIssueNumber: null,
        subIssueCount: 0,
        blockedByCount: 0,
        blockingCount: 0,
        latestBodyEdit: null,
        latestTitleEdit: null,
      },
    });
    const remoteSha = async (ref: string): Promise<string | null> => {
      const result = await git(checkout, ["ls-remote", "--heads", "origin", ref]);
      return result.length === 0 ? null : result.split("\t")[0]!;
    };
    try {
      const baseLedger = renderGitHubIssueJobLedger([]);
      await Deno.mkdir(`${checkout}/src`, { recursive: true });
      await Deno.mkdir(`${checkout}/docs`, { recursive: true });
      await new Deno.Command("git", { args: ["init", "--bare", remote] }).output();
      await git(checkout, ["init", "-b", "development"]);
      await git(checkout, ["config", "user.name", "Sentinel Test"]);
      await git(checkout, ["config", "user.email", "sentinel@example.invalid"]);
      await Deno.writeTextFile(`${checkout}/src/declared.ts`, "export const selected = 'base';\n");
      await Deno.writeTextFile(`${checkout}/docs/sentinel-issue-jobs.md`, baseLedger);
      await Deno.writeTextFile(`${checkout}/docs/sentinel-review-backlog.md`, "# Sentinel review backlog\n\n");
      await git(checkout, ["add", "src", "docs"]);
      await git(checkout, ["commit", "-m", "base"]);
      const baseSha = await git(checkout, ["rev-parse", "HEAD"]);
      await git(checkout, ["remote", "add", "origin", remote]);
      await git(checkout, ["push", "origin", "development"]);

      const branch = sentinelRecoveryCandidateBranch({
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "33177664067",
        source_revision: "a".repeat(64),
        candidate_generation: 1,
      });
      assert.match(branch, /^sentinel\/candidate-github_issue-33177664067-a{32}-g1-[0-9a-f]{16}$/u);
      await git(checkout, ["switch", "-c", branch]);
      await Deno.writeTextFile(`${checkout}/src/declared.ts`, "export const selected = 'reviewed';\n");
      await Deno.writeTextFile(`${checkout}/src/declared-new.ts`, "export const reviewOnly = true;\n");
      await Deno.writeTextFile(`${checkout}/docs/sentinel-issue-jobs.md`, `${baseLedger}\nreview-only ledger state\n`);
      await Deno.writeTextFile(`${checkout}/docs/sentinel-review-backlog.md`, "review-only backlog state\n");
      await git(checkout, ["add", "src", "docs"]);
      await git(checkout, ["commit", "-m", "reviewed candidate"]);
      const reviewedCandidateSha = await git(checkout, ["rev-parse", "HEAD"]);

      await git(checkout, [
        "restore",
        "--source",
        baseSha,
        "--staged",
        "--worktree",
        "--",
        "docs/sentinel-issue-jobs.md",
        "docs/sentinel-review-backlog.md",
      ]);
      await git(checkout, ["commit", "-m", "restore trusted controls for manual checkpoint"]);
      const checkpointSha = await git(checkout, ["rev-parse", "HEAD"]);
      assert.deepEqual(
        await assertGitHubIssueManualCheckpointCodeTreeEquivalent({
          checkout,
          baseSha,
          reviewedCandidateSha,
          checkpointSha,
          allowedPaths: manualIssue(136, "c").files,
        }),
        {
          reviewedCodePaths: ["src/declared-new.ts", "src/declared.ts"],
          checkpointCodePaths: ["src/declared-new.ts", "src/declared.ts"],
        },
      );
      assert.equal(
        await git(checkout, [
          "ls-tree",
          reviewedCandidateSha,
          "--",
          "src/declared.ts",
          "src/declared-new.ts",
        ]),
        await git(checkout, [
          "ls-tree",
          checkpointSha,
          "--",
          "src/declared.ts",
          "src/declared-new.ts",
        ]),
      );
      assert.equal(
        await git(checkout, ["show", `${checkpointSha}:docs/sentinel-issue-jobs.md`]),
        await git(checkout, ["show", `${baseSha}:docs/sentinel-issue-jobs.md`]),
      );
      assert.equal(
        await git(checkout, ["show", `${checkpointSha}:docs/sentinel-review-backlog.md`]),
        await git(checkout, ["show", `${baseSha}:docs/sentinel-review-backlog.md`]),
      );
      assert.equal(await remoteSha(`refs/heads/${branch}`), null);

      await git(checkout, ["switch", "development"]);
      await git(checkout, ["reset", "--hard", baseSha]);
      const checkpoint = { branch, sha: checkpointSha, baseSha };
      const issue = manualIssue(136, "c");
      const manualLedger = applyGitHubIssueJobDisposition(
        baseLedger,
        issue,
        baseSha,
        new Date("2026-08-28T00:30:00Z"),
        "manual_required",
        checkpoint,
      );
      assert.deepEqual(
        parseGitHubIssueJobLedger(manualLedger).map((entry) => ({
          number: entry.number,
          disposition: entry.disposition,
          checkpoint: entry.checkpoint,
        })),
        [{ number: 136, disposition: "manual_required", checkpoint }],
      );
      await Deno.writeTextFile(`${checkout}/docs/sentinel-issue-jobs.md`, manualLedger);
      await git(checkout, ["add", "docs/sentinel-issue-jobs.md"]);
      await git(checkout, ["commit", "-m", "record manual review requirement"]);
      const dispositionSha = await git(checkout, ["rev-parse", "HEAD"]);
      assert.equal(
        await git(checkout, ["rev-list", "--parents", "-n", "1", dispositionSha]),
        `${dispositionSha} ${baseSha}`,
      );
      assert.deepEqual(
        (await git(checkout, ["diff-tree", "--no-commit-id", "--name-only", "-r", dispositionSha]))
          .split("\n")
          .filter(Boolean),
        ["docs/sentinel-issue-jobs.md"],
      );

      const preReceiveLog = `${root}/pre-receive-updates`;
      await Deno.writeTextFile(
        `${remote}/hooks/pre-receive`,
        '#!/bin/sh\nif [ -n "$SENTINEL_TEST_PRE_RECEIVE_LOG" ]; then cat > "$SENTINEL_TEST_PRE_RECEIVE_LOG"; else cat > /dev/null; fi\n',
      );
      await Deno.chmod(`${remote}/hooks/pre-receive`, 0o700);
      let atomicPushStartingCalls = 0;
      let atomicPushAcceptedCalls = 0;
      await pushRetryPendingRefsAtomically({
        checkout,
        developmentSha: dispositionSha,
        checkpoint,
        expectedRemoteCheckpointSha: null,
        gitEnvironment: { SENTINEL_TEST_PRE_RECEIVE_LOG: preReceiveLog },
        onAtomicPushStarting: () => {
          atomicPushStartingCalls += 1;
          return Promise.resolve();
        },
        onAtomicPushAcceptedUnverified: () => {
          atomicPushAcceptedCalls += 1;
          return Promise.resolve();
        },
      });
      assert.equal(atomicPushStartingCalls, 1);
      assert.equal(atomicPushAcceptedCalls, 1);
      assert.equal(await remoteSha("refs/heads/development"), dispositionSha);
      assert.equal(await remoteSha(`refs/heads/${branch}`), checkpointSha);
      const publishedUpdates = new Map(
        (await Deno.readTextFile(preReceiveLog)).trim().split("\n").map((line) => {
          const [oldSha, newSha, ref] = line.split(" ");
          return [ref!, { oldSha: oldSha!, newSha: newSha! }];
        }),
      );
      assert.equal(publishedUpdates.size, 2);
      assert.deepEqual(publishedUpdates.get("refs/heads/development"), { oldSha: baseSha, newSha: dispositionSha });
      assert.deepEqual(publishedUpdates.get(`refs/heads/${branch}`), { oldSha: "0".repeat(40), newSha: checkpointSha });

      const racedBranch = sentinelRecoveryCandidateBranch({
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "33177664067",
        source_revision: "a".repeat(64),
        candidate_generation: 2,
      });
      await git(checkout, ["switch", "-c", racedBranch, "development"]);
      await Deno.writeTextFile(`${checkout}/src/declared.ts`, "export const selected = 'raced review';\n");
      await Deno.writeTextFile(`${checkout}/src/declared-new.ts`, "export const reviewOnly = 'raced';\n");
      await Deno.writeTextFile(`${checkout}/docs/sentinel-issue-jobs.md`, "transient candidate ledger state\n");
      await Deno.writeTextFile(`${checkout}/docs/sentinel-review-backlog.md`, "transient candidate backlog state\n");
      await git(checkout, ["add", "src", "docs"]);
      await git(checkout, ["commit", "-m", "second reviewed candidate"]);
      const racedReviewedCandidateSha = await git(checkout, ["rev-parse", "HEAD"]);
      await git(checkout, [
        "restore",
        "--source",
        dispositionSha,
        "--staged",
        "--worktree",
        "--",
        "docs/sentinel-issue-jobs.md",
        "docs/sentinel-review-backlog.md",
      ]);
      await git(checkout, ["commit", "-m", "restore trusted controls for raced manual checkpoint"]);
      const racedCheckpointSha = await git(checkout, ["rev-parse", "HEAD"]);
      await assertGitHubIssueManualCheckpointCodeTreeEquivalent({
        checkout,
        baseSha: dispositionSha,
        reviewedCandidateSha: racedReviewedCandidateSha,
        checkpointSha: racedCheckpointSha,
        allowedPaths: manualIssue(137, "d").files,
      });

      await git(checkout, ["switch", "development"]);
      const racedCheckpoint = { branch: racedBranch, sha: racedCheckpointSha, baseSha: dispositionSha };
      const racedLedger = applyGitHubIssueJobDisposition(
        await Deno.readTextFile(`${checkout}/docs/sentinel-issue-jobs.md`),
        manualIssue(137, "d"),
        dispositionSha,
        new Date("2026-08-28T00:40:00Z"),
        "manual_required",
        racedCheckpoint,
      );
      await Deno.writeTextFile(`${checkout}/docs/sentinel-issue-jobs.md`, racedLedger);
      await git(checkout, ["add", "docs/sentinel-issue-jobs.md"]);
      await git(checkout, ["commit", "-m", "record raced manual review requirement"]);
      const racedDispositionSha = await git(checkout, ["rev-parse", "HEAD"]);

      const competing = `${root}/competing`;
      await new Deno.Command("git", { args: ["clone", "--branch", "development", remote, competing] }).output();
      await git(competing, ["config", "user.name", "Sentinel Race Test"]);
      await git(competing, ["config", "user.email", "sentinel-race@example.invalid"]);
      let remoteAdvancedSha: string | null = null;
      let remoteRetained = false;
      let racedAtomicPushAccepted = false;
      await assert.rejects(
        () =>
          pushRetryPendingRefsAtomically({
            checkout,
            developmentSha: racedDispositionSha,
            checkpoint: racedCheckpoint,
            expectedRemoteCheckpointSha: null,
            gitEnvironment: { SENTINEL_TEST_PRE_RECEIVE_LOG: preReceiveLog },
            onAtomicPushStarting: async () => {
              await Deno.writeTextFile(`${competing}/competing.txt`, "remote development advanced\n");
              await git(competing, ["add", "competing.txt"]);
              await git(competing, ["commit", "-m", "advance development during sentinel atomic push"]);
              remoteAdvancedSha = await git(competing, ["rev-parse", "HEAD"]);
              await git(competing, ["push", "origin", "HEAD:refs/heads/development"]);
            },
            onAtomicPushAcceptedUnverified: () => {
              racedAtomicPushAccepted = true;
              return Promise.resolve();
            },
          }).then(() => {
            remoteRetained = true;
          }),
        /atomic push failed|git failed with/u,
      );
      assert.notEqual(remoteAdvancedSha, null);
      assert.equal(remoteRetained, false);
      assert.equal(racedAtomicPushAccepted, false);
      assert.equal(await remoteSha(`refs/heads/${racedBranch}`), null);
      assert.equal(await remoteSha("refs/heads/development"), remoteAdvancedSha);
      assert.notEqual(await remoteSha("refs/heads/development"), racedDispositionSha);
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "failed implementation snapshot preserves exact Git-visible candidate state as sidecars",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-failed-candidate-" });
    const checkout = `${root}/checkout`;
    const reportDirectory = `${root}/reports/failed-implementation-candidate`;
    const git = async (args: string[]): Promise<string> => {
      const output = await new Deno.Command("git", {
        args,
        cwd: checkout,
        stdout: "piped",
        stderr: "piped",
      }).output();
      if (!output.success) throw new Error(new TextDecoder().decode(output.stderr));
      return new TextDecoder().decode(output.stdout).trim();
    };
    try {
      await Deno.mkdir(checkout);
      await git(["init", "-b", "development"]);
      await Deno.writeTextFile(`${checkout}/modified.txt`, "before\n");
      await Deno.writeTextFile(`${checkout}/deleted.txt`, "delete me\n");
      await Deno.writeTextFile(`${checkout}/executable.sh`, "#!/bin/sh\nexit 0\n");
      await Deno.writeTextFile(`${checkout}/rename-source.txt`, "renamed bytes\n");
      await Deno.chmod(`${checkout}/executable.sh`, 0o755);
      await git(["add", "modified.txt", "deleted.txt", "executable.sh", "rename-source.txt"]);
      await git([
        "-c",
        "user.name=Sentinel Test",
        "-c",
        "user.email=sentinel@example.invalid",
        "commit",
        "-m",
        "base",
      ]);
      const baseSha = await git(["rev-parse", "HEAD"]);

      await Deno.writeTextFile(`${checkout}/committed-only.txt`, "committed candidate\n");
      await git(["add", "committed-only.txt"]);
      await git([
        "-c",
        "user.name=Sentinel Test",
        "-c",
        "user.email=sentinel@example.invalid",
        "commit",
        "-m",
        "candidate",
      ]);

      await Deno.writeTextFile(`${checkout}/modified.txt`, "after\n");
      await Deno.remove(`${checkout}/deleted.txt`);
      await Deno.writeTextFile(`${checkout}/executable.sh`, "#!/bin/sh\nexit 7\n");
      await Deno.writeFile(`${checkout}/new.bin`, new Uint8Array([0, 1, 254, 255]));
      await Deno.symlink("modified.txt", `${checkout}/linked.txt`);
      await Deno.rename(`${checkout}/rename-source.txt`, `${checkout}/rename-target.txt`);
      await captureFailedCandidateSnapshot(checkout, reportDirectory, baseSha);

      const snapshot = JSON.parse(await Deno.readTextFile(`${reportDirectory}/manifest.json`));
      assert.equal(snapshot.schema_version, 1);
      assert.equal(snapshot.base_sha, baseSha);
      assert.equal(snapshot.file_count, 8);
      const entries = new Map<string, Record<string, unknown>>(
        snapshot.files.map((entry: Record<string, unknown>) => [String(entry.path), entry]),
      );
      assert.deepEqual(entries.get("deleted.txt"), {
        path: "deleted.txt",
        source: "tracked",
        kind: "deleted",
      });
      const payload = async (path: string): Promise<Uint8Array> => {
        const entry = entries.get(path)!;
        return await Deno.readFile(`${reportDirectory}/${String(entry.payload)}`);
      };
      assert.equal(new TextDecoder().decode(await payload("modified.txt")), "after\n");
      assert.equal(entries.get("modified.txt")!.source, "tracked");
      assert.equal(new TextDecoder().decode(await payload("committed-only.txt")), "committed candidate\n");
      assert.equal(entries.get("committed-only.txt")!.source, "tracked");
      assert.deepEqual(await payload("new.bin"), new Uint8Array([0, 1, 254, 255]));
      assert.equal(entries.get("new.bin")!.source, "untracked");
      assert.equal(entries.get("linked.txt")!.kind, "symlink");
      assert.equal(new TextDecoder().decode(await payload("linked.txt")), "modified.txt");
      assert.equal(entries.get("rename-source.txt")!.kind, "deleted");
      assert.equal(entries.get("rename-target.txt")!.source, "untracked");
      assert.equal(new TextDecoder().decode(await payload("rename-target.txt")), "renamed bytes\n");
      assert.equal(Number(entries.get("executable.sh")!.mode) & 0o111, 0o111);
      assert.equal(new TextDecoder().decode(await payload("executable.sh")), "#!/bin/sh\nexit 7\n");
    } finally {
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "raw log capture and encrypted replay artifact export-load path preserves its contracts",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
    const deploymentWorkflow = await Deno.readTextFile(".github/workflows/deno-deploy.yml");
    assert.match(
      deploymentWorkflow,
      /deno-deploy-reusable\.yml@091927036712d54a020a3240b5b2fa492f78a94b/u,
    );
    assert.match(
      deploymentWorkflow,
      /sentinel_build_only == 'true'[\s\S]*'__sentinel_non_promoting__'/u,
      "Sentinel candidates must force the pinned reusable workflow into its mode without --prod",
    );
    assert.match(
      deploymentWorkflow,
      /preview_project:[\s\S]*sentinel_build_only == 'true'[\s\S]*'ai-ubq-fi'[\s\S]*'p-ai-ubq-fi'/u,
      "non-promoting mode must still target the exact requested Deno application",
    );
    assert.match(
      deploymentWorkflow,
      /name: sentinel-deployment-\$\{\{ github\.run_id \}\}[\s\S]*include-hidden-files: true/u,
    );

    const privateDir = await Deno.makeTempDir({ prefix: "sentinel-artifact-integration-" });
    try {
      const fakeDeno = `${privateDir}/fake-deno`;
      const rawLogs = `${privateDir}/raw.jsonl`;
      const expectedLogs = '{"event":"first","raw":"exact bytes"}\n{"event":"second","status":503}\n';
      await Deno.writeTextFile(
        fakeDeno,
        `#!/bin/sh\n` +
          `test "$DENO_DEPLOY_TOKEN" = "integration-token" || exit 41\n` +
          `test "$*" = "deploy logs --json --non-interactive --once --org ubiquity-dao --app ai-ubq-fi --start 2026-08-21T00:00:00.000Z --end 2026-08-21T00:20:00.000Z" || exit 42\n` +
          `printf '%s\\n' '{"event":"first","raw":"exact bytes"}' '{"event":"second","status":503}'\n`,
      );
      await Deno.chmod(fakeDeno, 0o700);
      await captureRawDenoLogs({
        cwd: privateDir,
        token: "integration-token",
        organization: "ubiquity-dao",
        app: "ai-ubq-fi",
        start: "2026-08-21T00:00:00.000Z",
        end: "2026-08-21T00:20:00.000Z",
        destination: rawLogs,
        executable: fakeDeno,
      });
      assert.equal(await Deno.readTextFile(rawLogs), expectedLogs);

      const replayCasesDir = `${privateDir}/replay-cases`;
      const replayIndexDir = `${privateDir}/replay-index`;
      const githubEnvironmentPath = `${privateDir}/github-environment`;
      await Deno.mkdir(replayCasesDir);
      await Deno.mkdir(replayIndexDir);
      const current = capture("current", "1", "a", Date.parse("2026-08-21T00:01:00.000Z"));
      await writeReplayArtifactMetadata({
        captures: [current],
        replayCasesDir,
        replayIndexDir,
        runId: "12345",
        githubEnvironmentPath,
      });
      const artifactName = `${replayIndexArtifactName([current.manifest.case_group_digest])}-12345`;
      assert.deepEqual(JSON.parse(await Deno.readTextFile(`${replayCasesDir}/captures.json`)), {
        schema_version: 1,
        captures: [current],
      });
      assert.deepEqual(JSON.parse(await Deno.readTextFile(`${replayIndexDir}/index.json`)), {
        schema_version: 1,
        replay_artifact_name: artifactName,
        cases: [{
          fingerprint: current.manifest.fingerprint,
          case_group_digest: current.manifest.case_group_digest,
          captured_at_ms: current.manifest.captured_at_ms,
        }],
      });
      assert.equal(
        await Deno.readTextFile(githubEnvironmentPath),
        `SENTINEL_HAS_REPLAY_CASES=true\nSENTINEL_REPLAY_BUNDLE_ARTIFACT_NAME=${artifactName}\n`,
      );

      const archiveRoot = `${privateDir}/archive`;
      await Deno.mkdir(`${archiveRoot}/replay-cases`, { recursive: true });
      const retainedMatching = capture("retained-matching", "2", "a", Date.parse("2026-08-20T00:01:00.000Z"));
      const retainedUnrelated = capture("retained-unrelated", "3", "b", Date.parse("2026-08-20T00:02:00.000Z"));
      await Deno.writeTextFile(
        `${archiveRoot}/replay-cases/captures.json`,
        `${JSON.stringify({ schema_version: 1, captures: [retainedMatching, retainedUnrelated] })}\n`,
      );
      const archivePath = `${privateDir}/artifact.zip`;
      const zip = await new Deno.Command("/usr/bin/zip", {
        args: ["-q", archivePath, "replay-cases/captures.json"],
        cwd: archiveRoot,
        stdout: "null",
        stderr: "piped",
      }).output();
      assert.equal(zip.success, true, new TextDecoder().decode(zip.stderr));
      const archive = await Deno.readFile(archivePath);
      const artifact: GitHubArtifact = {
        id: 7,
        name: `${replayIndexArtifactName([current.manifest.case_group_digest])}-older-run`,
        sizeInBytes: archive.byteLength,
        expired: false,
        createdAt: "2026-08-20T00:03:00.000Z",
        expiresAt: "2026-11-18T00:03:00.000Z",
      };
      const github = {
        listRepositoryArtifacts: () => Promise.resolve([artifact]),
        downloadArtifact: (artifactId: number) => {
          assert.equal(artifactId, artifact.id);
          return Promise.resolve(archive);
        },
      } as unknown as GitHubActionsClient;
      const loaded = await loadMatchingRetainedCaptures({
        github,
        current: [current],
        privateDir,
        nowMs: Date.parse("2026-08-21T00:20:00.000Z"),
      });
      assert.deepEqual(loaded, [retainedMatching]);
    } finally {
      await Deno.remove(privateDir, { recursive: true });
    }
  },
});
