import assert from "node:assert/strict";
import { encryptSentinelArtifact, type SentinelArtifactFile } from "../scripts/sentinel/artifact-crypto.ts";
import {
  buildSentinelRecoveryDraftPullRequest,
  createOrReuseSentinelRecoveryDraftPullRequest,
  isSentinelArtifactRecoveryEligible,
  legacyArtifactNeedsManualDisposition,
  manualRecoveryRecordForLegacyArtifact,
  recoverSentinelArtifactCandidate,
  selectSentinelRecoveryArtifacts,
  sentinelRecoveryCandidateBranch,
} from "../scripts/sentinel/artifact-recovery.ts";
import type { GitHubArtifact } from "../scripts/sentinel/github.ts";
import type { SentinelRecoveryRecordV1 } from "../scripts/sentinel/recovery.ts";

const permissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run" }),
]);
const unavailable = permissions.some((permission) => permission.state !== "granted");
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

const sha256 = async (bytes: Uint8Array): Promise<string> =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes))))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");

const git = async (cwd: string, args: readonly string[]): Promise<string> => {
  const output = await new Deno.Command("git", {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
  }).output();
  if (!output.success) throw new Error(textDecoder.decode(output.stderr));
  return textDecoder.decode(output.stdout).trim();
};

const createBaseCheckout = async (): Promise<Readonly<{ root: string; checkout: string; baseSha: string }>> => {
  const root = await Deno.makeTempDir({ prefix: "sentinel-artifact-recovery-test-" });
  const checkout = `${root}/checkout`;
  await Deno.mkdir(checkout, { recursive: true });
  await git(checkout, ["init", "-b", "development"]);
  await git(checkout, ["config", "user.name", "Sentinel Fixture"]);
  await git(checkout, ["config", "user.email", "sentinel-fixture@example.invalid"]);
  await Deno.writeTextFile(`${checkout}/README.md`, "base\n");
  await git(checkout, ["add", "README.md"]);
  await git(checkout, ["commit", "-m", "base"]);
  return { root, checkout, baseSha: await git(checkout, ["rev-parse", "HEAD"]) };
};

const makeRecord = (baseSha: string, overrides: Partial<SentinelRecoveryRecordV1> = {}): SentinelRecoveryRecordV1 => ({
  schema_version: 1,
  identity: {
    repository: "ubiquity/ai.ubq.fi",
    source_kind: "github_issue",
    source_id: "136",
    source_revision: "a".repeat(64),
    candidate_generation: 1,
  },
  run_id: "33197180235",
  attempt: 1,
  lease_token: "artifact-fixture-lease",
  base_sha: baseSha,
  phase: "recovery_pending",
  disposition: "active",
  state_version: 1,
  created_at: "2026-08-28T18:00:00.000Z",
  updated_at: "2026-08-28T18:00:00.000Z",
  candidate_branch: null,
  candidate_sha: null,
  changed_files: [],
  tree_sha: null,
  failure_class: null,
  failure_fingerprint: null,
  artifact_ids: [],
  artifact_digests: [],
  reason: "encrypted candidate awaiting recovery",
  next_action: "reconstruct the exact base and candidate patch",
  predecessor: null,
  ...overrides,
});

const makeEncryptedFixture = async (
  baseSha: string,
  manifestBaseSha = baseSha,
): Promise<Readonly<{ encrypted: Uint8Array<ArrayBuffer>; key: Uint8Array<ArrayBuffer>; secret: string }>> => {
  const secret = "candidate-private-fixture-plaintext";
  const payload = textEncoder.encode("recovered candidate\n");
  const manifest = {
    schema_version: 1,
    base_sha: manifestBaseSha,
    captured_at: "2026-08-28T18:01:00.000Z",
    file_count: 1,
    total_bytes: payload.byteLength,
    files: [{
      path: "candidate.txt",
      source: "untracked",
      kind: "file",
      mode: 0o100644,
      size: payload.byteLength,
      sha256: await sha256(payload),
      payload: "files/0000.bin",
    }],
  };
  const files: SentinelArtifactFile[] = [
    {
      path: "raw-logs/private.log",
      bytes: textEncoder.encode(secret),
    },
    {
      path: "reports/failed-implementation-candidate/files/0000.bin",
      bytes: payload,
    },
    {
      path: "reports/failed-implementation-candidate/manifest.json",
      bytes: textEncoder.encode(JSON.stringify(manifest)),
    },
  ];
  const key = new Uint8Array(32).fill(23);
  const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(9));
  payload.fill(0);
  for (const file of files) file.bytes.fill(0);
  return { encrypted, key, secret };
};

const makeArtifact = (id: number, createdAt: string, overrides: Partial<GitHubArtifact> = {}): GitHubArtifact => ({
  id,
  name: `sentinel-evidence-v1-${id}`,
  sizeInBytes: 1,
  expired: false,
  createdAt,
  expiresAt: null,
  ...overrides,
});

const makeEligibilityEvidence = (
  status: string,
  extraReports: Readonly<Record<string, unknown>> = {},
): SentinelArtifactFile[] => {
  const baseSha = "a".repeat(40);
  const files: SentinelArtifactFile[] = [
    {
      path: "reports/cycle.json",
      bytes: textEncoder.encode(JSON.stringify({
        schema_version: 1,
        run_id: "33197180235",
        status,
        stage: status === "failed" ? "failed" : "complete",
        started_at: "2026-08-28T18:00:00.000Z",
        base_development_sha: baseSha,
      })),
    },
    {
      path: "reports/failed-implementation-candidate/manifest.json",
      bytes: textEncoder.encode("{}"),
    },
  ];
  for (const [path, report] of Object.entries(extraReports)) {
    files.push({ path, bytes: textEncoder.encode(JSON.stringify(report)) });
  }
  return files;
};

Deno.test({
  name: "artifact selection keeps the newest bounded evidence window",
  ignore: unavailable,
  fn() {
    const artifacts = Array.from({ length: 65 }, (_, index) =>
      makeArtifact(
        10_000 + index,
        new Date(Date.UTC(2026, 7, 28, 18, index)).toISOString(),
      ));
    artifacts.push(
      makeArtifact(20_000, new Date(Date.UTC(2026, 7, 28, 19, 10)).toISOString(), { expired: true }),
      makeArtifact(20_001, new Date(Date.UTC(2026, 7, 28, 19, 11)).toISOString(), {
        name: "not-sentinel-evidence",
      }),
    );
    const selected = selectSentinelRecoveryArtifacts(artifacts);
    assert.equal(selected.length, 64);
    assert.deepEqual(
      selected.map((artifact) => artifact.id),
      Array.from({ length: 64 }, (_, index) => 10_064 - index),
    );
  },
});

Deno.test({
  name: "artifact recovery skips terminal and successful cycle evidence without eligibility",
  ignore: unavailable,
  fn() {
    for (const status of ["no_change", "observed", "preview_complete", "preview_rolled_back", "kept", "rolled_back"]) {
      const files = makeEligibilityEvidence(status);
      try {
        assert.equal(isSentinelArtifactRecoveryEligible(files), false, status);
      } finally {
        for (const file of files) file.bytes.fill(0);
      }
    }
    for (
      const [path, report] of [
        ["reports/github-issue-disposition.json", { disposition: "manual_required" }],
        ["reports/github-issue-disposition.json", { disposition: "rejected" }],
        ["reports/github-issue-disposition.json", { disposition: "resolved" }],
        ["reports/recovery-record.json", { phase: "delivered", disposition: "delivered" }],
        ["reports/github-issue-production-outcome.json", { outcome: "kept" }],
        ["reports/github-issue-manual-checkpoint.json", { phase: "native_review_exhausted" }],
      ] as const
    ) {
      const files = makeEligibilityEvidence("failed", { [path]: report });
      try {
        assert.equal(isSentinelArtifactRecoveryEligible(files), false, path);
      } finally {
        for (const file of files) file.bytes.fill(0);
      }
    }
    const failed = makeEligibilityEvidence("failed");
    const runningWithoutFailure = makeEligibilityEvidence("running");
    const timedOut = makeEligibilityEvidence("running", {
      "reports/failure.json": { codex_timed_out: true },
    });
    try {
      assert.equal(isSentinelArtifactRecoveryEligible(failed), true);
      assert.equal(isSentinelArtifactRecoveryEligible(runningWithoutFailure), false);
      assert.equal(isSentinelArtifactRecoveryEligible(timedOut), true);
      assert.equal(
        isSentinelArtifactRecoveryEligible(runningWithoutFailure, { status: "completed", conclusion: "failure" }),
        true,
      );
      assert.equal(
        isSentinelArtifactRecoveryEligible(runningWithoutFailure, { status: "completed", conclusion: "success" }),
        false,
      );
    } finally {
      for (const file of [...failed, ...runningWithoutFailure, ...timedOut]) file.bytes.fill(0);
    }
  },
});

Deno.test("authenticated legacy candidate evidence receives a durable manual disposition", () => {
  const headSha = "b".repeat(40);
  const digest = `sha256:${"c".repeat(64)}`;
  const record = manualRecoveryRecordForLegacyArtifact(
    "ubiquity/ai.ubq.fi",
    makeArtifact(9697049137, "2026-08-28T17:58:52.000Z", {
      workflowRunId: 33197180235,
      workflowRunHeadSha: headSha,
    }),
    digest,
  );
  assert(record);
  assert.equal(record.phase, "manual_required");
  assert.equal(record.disposition, "manual_required");
  assert.deepEqual(record.artifact_ids, [9697049137]);
  assert.deepEqual(record.artifact_digests, [digest]);
  assert.equal(record.identity.source_id, "33197180235");
  assert.equal(record.identity.source_revision, headSha);
  assert.match(record.next_action ?? "", /repository owner/u);
});

Deno.test("failed legacy candidate evidence remains classifiable when its cycle schema is incomplete", () => {
  const files = makeEligibilityEvidence("legacy_unknown");
  try {
    assert.equal(isSentinelArtifactRecoveryEligible(files), false);
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "failure" }),
      true,
    );
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "success" }),
      false,
    );
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test("failed authenticated legacy reports without a candidate still receive manual disposition", () => {
  const files = [
    { path: "reports/cycle.json", bytes: new TextEncoder().encode('{"schema_version":0,"status":"legacy"}') },
  ];
  try {
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "failure" }),
      true,
    );
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "success" }),
      false,
    );
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test({
  name: "encrypted candidate recovery creates one deterministic quarantined commit and draft PR request",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const artifact = await makeEncryptedFixture(fixture.baseSha);
    const record = makeRecord(fixture.baseSha);
    try {
      const first = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: artifact.encrypted,
        keyBytes: artifact.key,
        record,
        expectedBaseSha: fixture.baseSha,
        artifactId: 9697049137,
      });
      const second = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: artifact.encrypted,
        keyBytes: artifact.key,
        record,
        expectedBaseSha: fixture.baseSha,
        artifactId: 9697049137,
      });
      assert.equal(first.disposition, "recovered");
      assert.equal(first.reason, "recovered");
      assert.equal(first.candidate_sha, second.candidate_sha);
      assert.equal(first.tree_sha, second.tree_sha);
      assert.equal(first.candidate_branch, sentinelRecoveryCandidateBranch(record));
      assert.match(first.candidate_branch ?? "", /^sentinel\/candidate-github_issue-136-a{32}-g1-[0-9a-f]{16}$/u);
      assert.notEqual(
        first.candidate_branch,
        sentinelRecoveryCandidateBranch({
          ...record,
          identity: { ...record.identity, candidate_generation: 2 },
        }),
      );
      assert.deepEqual(first.changed_files, ["candidate.txt"]);
      assert.equal(first.recovery_record?.phase, "checkpoint_durable");
      assert.equal(first.recovery_record?.candidate_sha, first.candidate_sha);
      assert.equal(first.draft_pull_request?.body.draft, true);
      assert.equal(first.draft_pull_request?.body.maintainer_can_modify, false);
      assert.equal(first.draft_pull_request?.auto_merge, false);
      assert.equal(first.draft_pull_request?.autoMergeEnabled, false);
      assert.equal(JSON.stringify(first).includes(artifact.secret), false);

      const candidateSha = first.candidate_sha!;
      assert.equal(await git(fixture.checkout, ["show", `${candidateSha}:candidate.txt`]), "recovered candidate");
      assert.equal(
        await git(fixture.checkout, ["rev-list", "--parents", "-n", "1", candidateSha]),
        `${candidateSha} ${fixture.baseSha}`,
      );
      assert.equal(await git(fixture.checkout, ["rev-parse", `refs/heads/${first.candidate_branch}`]), candidateSha);

      const request = buildSentinelRecoveryDraftPullRequest({
        repository: record.identity.repository,
        record: first.recovery_record!,
        candidateBranch: first.candidate_branch!,
        candidateSha,
        artifactId: 9697049137,
      });
      const requests: Array<{ url: string; init: RequestInit }> = [];
      const pull = await createOrReuseSentinelRecoveryDraftPullRequest({
        token: "fixture-token",
        request,
        fetcher: (input, init = {}) => {
          const url = String(input);
          requests.push({ url, init });
          if (init.method === "GET") {
            return Promise.resolve(Response.json([{
              number: 99,
              html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/99",
              state: "open",
              merged_at: null,
              head: { ref: "unrelated", sha: "f".repeat(40) },
              base: { ref: "development" },
              draft: false,
              auto_merge: null,
              body: null,
            }]));
          }
          const body = JSON.parse(String(init.body));
          assert.equal(body.draft, true);
          assert.equal(body.maintainer_can_modify, false);
          assert.equal(Object.hasOwn(body, "auto_merge"), false);
          return Promise.resolve(
            new Response(
              JSON.stringify({
                number: 1,
                html_url: "https://github.com/ubiquity/ai.ubq.fi/pull/1",
                state: "open",
                merged_at: null,
                head: { ref: request.body.head, sha: candidateSha },
                base: { ref: "development" },
                draft: true,
                auto_merge: null,
                body: body.body,
              }),
              { status: 201, headers: { "content-type": "application/json" } },
            ),
          );
        },
      });
      assert.equal(pull.number, 1);
      assert.equal(pull.draft, true);
      assert.equal(pull.autoMergeEnabled, false);
      assert.equal(requests.length, 2);
      assert.match(requests[0]!.url, /state=open/);
      assert.equal(requests[1]!.init.method, "POST");
    } finally {
      artifact.encrypted.fill(0);
      artifact.key.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "corrupt and wrong-base encrypted evidence become manual_required without plaintext leakage",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const artifact = await makeEncryptedFixture(fixture.baseSha);
    const record = makeRecord(fixture.baseSha);
    const corrupt = artifact.encrypted.slice();
    corrupt[0] ^= 1;
    try {
      const corruptResult = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: corrupt,
        keyBytes: artifact.key,
        record,
      });
      assert.equal(corruptResult.disposition, "manual_required");
      assert.equal(corruptResult.reason, "artifact_corrupt");
      assert.equal(corruptResult.candidate_sha, null);
      assert.equal(JSON.stringify(corruptResult).includes(artifact.secret), false);

      const staleBase = "c".repeat(40);
      const staleArtifact = await makeEncryptedFixture(fixture.baseSha, staleBase);
      try {
        const wrongBaseResult = await recoverSentinelArtifactCandidate({
          checkout: fixture.checkout,
          encryptedBytes: staleArtifact.encrypted,
          keyBytes: staleArtifact.key,
          record,
        });
        assert.equal(wrongBaseResult.disposition, "manual_required");
        assert.equal(wrongBaseResult.reason, "artifact_wrong_base");
        assert.equal(wrongBaseResult.candidate_branch, null);
        assert.equal(wrongBaseResult.candidate_sha, null);
        assert.equal(JSON.stringify(wrongBaseResult).includes(staleArtifact.secret), false);
      } finally {
        staleArtifact.encrypted.fill(0);
        staleArtifact.key.fill(0);
      }
    } finally {
      corrupt.fill(0);
      artifact.encrypted.fill(0);
      artifact.key.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "artifact recovery workflow job runs after the sentinel and keeps evidence ciphertext-only",
  ignore: unavailable,
  async fn() {
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    const jobStart = workflow.indexOf("\n  artifact-recovery:");
    assert.notEqual(jobStart, -1);
    const job = workflow.slice(jobStart);
    assert.match(job, /\n\s{4}needs: converge\n/u);
    assert.match(job, /always\(\)/u);
    assert.match(job, /SENTINEL_ARTIFACT_KEY: \$\{\{ secrets\.SENTINEL_ARTIFACT_KEY \}\}/u);
    assert.match(job, /scripts\/sentinel\/artifact-recovery\.ts/u);
    assert.match(job, /draft-only|draft recovery/iu);
    assert.doesNotMatch(job, /auto[_-]merge\s*[:=]\s*true/iu);
  },
});
