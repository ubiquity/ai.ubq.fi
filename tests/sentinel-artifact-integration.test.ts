import assert from "node:assert/strict";
import type { GitHubActionsClient, GitHubArtifact } from "../scripts/sentinel/github.ts";
import {
  aggregateCandidateChangedPaths,
  captureFailedCandidateSnapshot,
  loadMatchingRetainedCaptures,
  prepareResumedGitHubIssueCandidate,
  pushImmutableTemporaryCheckpoint,
  replayIndexArtifactName,
  requireResolvedReviewBacklogImplementation,
  RetryCheckpointResumeError,
  retryCheckpointResumeFailureDisposition,
  writeReplayArtifactMetadata,
} from "../scripts/sentinel/main.ts";
import { requireExactRemoteCandidateBranch } from "../scripts/sentinel/issue-pr-pre-push.ts";
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
  name: "retry checkpoint publication creates one immutable remote ref",
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
      await git(["push", "origin", `${baseSha}:refs/heads/sentinel/candidate-201`]);
      await assert.rejects(
        () => pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-201", {}, null),
        /does not match the exact previously pushed SHA/,
      );
      await assert.rejects(
        () => pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-201", {}, "f".repeat(40)),
        /does not match the exact previously pushed SHA/,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-201"]))
          .split("\t")[0],
        baseSha,
      );
      assert.equal(
        await pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-201", {}, baseSha),
        checkpointSha,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-201"]))
          .split("\t")[0],
        checkpointSha,
      );
      assert.equal(
        await pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-201", {}, checkpointSha),
        checkpointSha,
      );
      await requireExactRemoteCandidateBranch(checkout, "sentinel/candidate-201", checkpointSha);
      await git(["push", "--force", "origin", `${baseSha}:refs/heads/sentinel/candidate-201`]);
      await assert.rejects(
        () => requireExactRemoteCandidateBranch(checkout, "sentinel/candidate-201", checkpointSha),
        /does not match its recorded SHA/,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-201"]))
          .split("\t")[0],
        baseSha,
      );
      await git(["push", "--force", "origin", `${checkpointSha}:refs/heads/sentinel/candidate-201`]);
      await requireExactRemoteCandidateBranch(checkout, "sentinel/candidate-201", checkpointSha);

      await git(["switch", "-c", "sentinel/candidate-202"]);
      assert.equal(
        await pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-202", {}, null),
        checkpointSha,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-202"]))
          .split("\t")[0],
        checkpointSha,
      );

      await git(["push", "origin", `${checkpointSha}:refs/heads/sentinel/candidate-203`]);
      await git(["switch", "-c", "divergent-checkpoint", baseSha]);
      await Deno.writeTextFile(`${checkout}/allowed.txt`, "divergent\n");
      await git(["add", "allowed.txt"]);
      await git(["commit", "-m", "divergent checkpoint"]);
      await assert.rejects(
        () => pushImmutableTemporaryCheckpoint(checkout, "sentinel/candidate-203", {}, checkpointSha),
        /does not descend from the exact previously pushed SHA/,
      );
      assert.equal(
        (await git(["ls-remote", "--heads", "origin", "refs/heads/sentinel/candidate-203"]))
          .split("\t")[0],
        checkpointSha,
      );
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
