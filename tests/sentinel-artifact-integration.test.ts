import assert from "node:assert/strict";
import type { GitHubActionsClient, GitHubArtifact } from "../scripts/sentinel/github.ts";
import {
  loadMatchingRetainedCaptures,
  replayIndexArtifactName,
  writeReplayArtifactMetadata,
} from "../scripts/sentinel/main.ts";
import { captureRawDenoLogs } from "../scripts/sentinel/validation.ts";
import { type ExportedSentinelReplayCapture, SENTINEL_REPLAY_TTL_MS } from "../src/sentinel_replay_capture.ts";

const requiredPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);

const encodeBase64Url = (bytes: Uint8Array): string =>
  btoa(String.fromCharCode(...bytes)).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");

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
  name: "raw log capture and encrypted replay artifact export-load path preserves its contracts",
  ignore: requiredPermissions.some((permission) => permission.state !== "granted"),
  async fn() {
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
