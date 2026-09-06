import assert from "node:assert/strict";
import { encryptSentinelArtifact, type SentinelArtifactFile } from "../scripts/sentinel/artifact-crypto.ts";
import {
  authenticatedMatrixCellReport,
  buildSentinelRecoveryDraftPullRequest,
  createOrReuseSentinelRecoveryDraftPullRequest,
  currentRecoveryDevelopmentHead,
  isSentinelArtifactRecoveryEligible,
  legacyArtifactHasTerminalReport,
  legacyArtifactNeedsManualDisposition,
  legacyArtifactTerminalDisposition,
  manualRecoveryRecordForLegacyArtifact,
  matrixCellRecoveryRecordFromArtifact,
  matrixEvidenceRetentionRecord,
  recoverSentinelArtifactCandidate,
  recoverSentinelArtifactsInActions,
  resolveMatrixRecoveryGeneration,
  selectSentinelRecoveryArtifacts,
  sentinelRecoveryCandidateBranch,
  terminalRecoveryRecordForLegacyArtifact,
} from "../scripts/sentinel/artifact-recovery.ts";
import type { GitHubArtifact } from "../scripts/sentinel/github.ts";
import {
  matrixCellRecoverySourceRevision,
  matrixCellWorkSelectionFromArtifact,
} from "../scripts/sentinel/matrix-cell.ts";
import { matrixCellReportDigest } from "../scripts/sentinel/matrix.ts";
import { parseSentinelRecoveryRecord, type SentinelRecoveryRecordV1 } from "../scripts/sentinel/recovery.ts";
import { decryptSentinelEvidenceEnvelope } from "../scripts/sentinel/artifact-recovery.ts";
import { githubIssuePlanDigest } from "../scripts/sentinel/issue-delivery.ts";
import { issueJobFindingId } from "../scripts/sentinel/issues.ts";
import { loadRetainedIssueFrozenPlan } from "../scripts/sentinel/main.ts";
import type { SentinelRetryDecision } from "../scripts/sentinel/retry.ts";
import {
  parseSentinelRecoveryLedger,
  resolveSentinelRecoverySelection,
  sentinelRecoveryIdentityKey,
  type SentinelRecoveryLedgerV1,
} from "../scripts/sentinel/recovery-ledger.ts";

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

const MATRIX_CELL_ID = `cell-${"9".repeat(64)}`;
const MATRIX_WORK_SELECTION = {
  source_kind: "github_issue" as const,
  source_id: "208",
  source_revision: "a".repeat(64),
};

type MatrixFixture = Readonly<{
  files: SentinelArtifactFile[];
  record: SentinelRecoveryRecordV1;
  secret: string;
  cell: Record<string, unknown>;
  workSelection: Readonly<typeof MATRIX_WORK_SELECTION> | null;
}>;

const matrixCellEvidence = async (
  fixture: Readonly<{ baseSha: string }>,
  options: Readonly<{
    workSelection?: Readonly<typeof MATRIX_WORK_SELECTION> | null;
    payloadText?: string;
    cellId?: string;
    findingId?: string;
    findingFingerprint?: string;
    allowedPath?: string;
    changedPaths?: readonly string[];
  }> = {},
): Promise<MatrixFixture> => {
  const secret = "matrix-candidate-private-plaintext";
  const payload = textEncoder.encode(options.payloadText ?? "matrix recovered candidate\n");
  const workSelection = options.workSelection === undefined ? MATRIX_WORK_SELECTION : options.workSelection;
  const cellId = options.cellId ?? MATRIX_CELL_ID;
  const findingId = options.findingId ?? "fixture";
  const findingFingerprint = options.findingFingerprint ?? "4".repeat(64);
  const allowedPath = options.allowedPath ?? "candidate.txt";
  const changedPaths = options.changedPaths ?? [allowedPath];
  const cell = {
    cell_id: cellId,
    finding_ids: [findingId],
    finding_fingerprints: [findingFingerprint],
    allowed_paths: [allowedPath],
    prohibited_paths: [],
    shared_paths: [],
    dependencies: [],
    validation_requirements: ["fixture validation"],
    base_sha: fixture.baseSha,
    branch: `sentinel/candidate-33197770000-1-${cellId}`,
    report_path: `.sentinel/reports/matrix/33197770000-1/${cellId}.json`,
    artifact_name: `sentinel-matrix-cell-v1-33197770000-1-${cellId}`,
  };
  const sourceRevision = await matrixCellRecoverySourceRevision(fixture.baseSha, cell, workSelection);
  const record = parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: workSelection?.source_kind ?? ("triage" as const),
      source_id: workSelection?.source_id ?? cellId,
      source_revision: sourceRevision,
      candidate_generation: 1,
    },
    run_id: "33197770000",
    attempt: 1,
    lease_token: `matrix-cell-33197770000-1-${cellId}`,
    base_sha: fixture.baseSha,
    phase: "recovery_pending",
    disposition: "active",
    state_version: 1,
    created_at: "2026-08-28T19:00:00.000Z",
    updated_at: "2026-08-28T19:00:00.000Z",
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [...changedPaths],
    tree_sha: null,
    failure_class: "transient_transport",
    failure_fingerprint: "8".repeat(64),
    artifact_ids: [],
    artifact_digests: [],
    reason: "The matrix cell ended retry_pending without a durable candidate ref.",
    next_action: "Recover the encrypted cell candidate evidence into a quarantined branch.",
    predecessor: null,
  });
  const unsignedReport = {
    schema_version: 1 as const,
    run_id: "33197770000",
    run_attempt: 1,
    plan_digest: "3".repeat(64),
    cell_id: cellId,
    base_sha: fixture.baseSha,
    branch: `sentinel/candidate-33197770000-1-${cellId}`,
    head_sha: null,
    tree_sha: null,
    changed_paths: [...changedPaths],
    finding_dispositions: [{
      finding_id: findingId,
      fingerprint: findingFingerprint,
      status: "blocked" as const,
      summary: "Cell did not complete within the bounded continuation.",
      changed_files: [],
      validation: [],
    }],
    validation: {
      passed: false,
      checks: [{ name: "cell-outcome", passed: false, detail: "The bounded Luna attempt timed out." }],
    },
    replay: { attempted: false, passed: true, results: [] },
    status: "retry_pending" as const,
    failure_reason: "The bounded Luna attempt timed out.",
    artifact_sha256: null,
    report_digest: "0".repeat(64),
  };
  const report = { ...unsignedReport, report_digest: await matrixCellReportDigest(unsignedReport) };
  const manifest = {
    schema_version: 1,
    plan_digest: "3".repeat(64),
    run_id: "33197770000",
    run_attempt: 1,
    base_sha: fixture.baseSha,
    captured_at: "2026-08-28T19:00:01.000Z",
    cell_contract: cell,
    capture_attestation: {
      schema_version: 1,
      cell_status: "retry_pending",
      secret_scan_path: null,
      secret_scan_sha256: null,
    },
    file_count: changedPaths.length,
    total_bytes: changedPaths.length === 0 ? 0 : payload.byteLength,
    files: changedPaths.length === 0 ? [] : [{
      path: allowedPath,
      source: "untracked",
      kind: "file",
      mode: 0o100644,
      size: payload.byteLength,
      sha256: await sha256(payload),
      payload: "files/0000.bin",
    }],
  };
  const files: SentinelArtifactFile[] = [
    { path: `reports/matrix/${cellId}/cell.json`, bytes: textEncoder.encode(JSON.stringify(report)) },
    {
      path: `reports/matrix/${cellId}/recovery-record.json`,
      bytes: textEncoder.encode(JSON.stringify(record)),
    },
    { path: `reports/matrix/${cellId}/manifest.json`, bytes: textEncoder.encode(JSON.stringify(manifest)) },
  ];
  if (changedPaths.length > 0) {
    files.push({ path: `reports/matrix/${cellId}/files/0000.bin`, bytes: payload });
  }
  if (workSelection !== null) {
    files.push({
      path: `reports/matrix/${cellId}/work-selection.json`,
      bytes: textEncoder.encode(JSON.stringify({ schema_version: 1, ...workSelection })),
    });
  }
  return { files, record, secret, cell, workSelection };
};

const matrixFilesWith = (
  files: readonly SentinelArtifactFile[],
  path: string,
  mutate: (value: Record<string, unknown>) => Record<string, unknown>,
): SentinelArtifactFile[] =>
  files.map((file) =>
    file.path === path
      ? {
        path: file.path,
        bytes: textEncoder.encode(JSON.stringify(mutate(JSON.parse(textDecoder.decode(file.bytes))))),
      }
      : file
  );

Deno.test({
  name: "authenticated matrix cell retry evidence is recognized only with an exact binding",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const evidence = await matrixCellEvidence(fixture);
    try {
      const recognized = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
      assert(recognized);
      assert.equal(recognized.identity.source_kind, "github_issue");
      assert.equal(recognized.identity.source_id, "208");
      assert.equal(recognized.identity.source_revision, evidence.record.identity.source_revision);
      assert.equal(recognized.identity.candidate_generation, 1);
      assert.equal(recognized.phase, "recovery_pending");
      assert.deepEqual(recognized.changed_files, ["candidate.txt"]);
      assert.equal(recognized.base_sha, fixture.baseSha);
      assert.equal(await matrixCellRecoveryRecordFromArtifact(evidence.files, "other/private-repo"), null);
      const changedStatus = matrixFilesWith(evidence.files, `reports/matrix/${MATRIX_CELL_ID}/cell.json`, (value) => ({
        ...value,
        status: "succeeded",
        head_sha: "7".repeat(40),
        tree_sha: "7".repeat(40),
        artifact_sha256: "7".repeat(64),
        failure_reason: null,
      }));
      assert.equal(await matrixCellRecoveryRecordFromArtifact(changedStatus, "ubiquity/ai.ubq.fi"), null);
      const tamperedPhase = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/recovery-record.json`,
        (value) => ({ ...value, phase: "checkpoint_durable", disposition: "active" }),
      );
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedPhase, "ubiquity/ai.ubq.fi"), null);
      const tamperedDigest = matrixFilesWith(evidence.files, `reports/matrix/${MATRIX_CELL_ID}/cell.json`, (value) => ({
        ...value,
        report_digest: "5".repeat(64),
      }));
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedDigest, "ubiquity/ai.ubq.fi"), null);
      const tamperedPlan = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
        (value) => ({ ...value, plan_digest: "5".repeat(64) }),
      );
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedPlan, "ubiquity/ai.ubq.fi"), null);
      const tamperedPaths = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
        (value) => ({
          ...value,
          files: [{ ...(value.files as Array<Record<string, unknown>>)[0]!, path: "outside.txt" }],
        }),
      );
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedPaths, "ubiquity/ai.ubq.fi"), null);
      const tamperedLease = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/recovery-record.json`,
        (value) => ({ ...value, lease_token: "matrix-cell-33197770000-2-matrix-cell-tampered" }),
      );
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedLease, "ubiquity/ai.ubq.fi"), null);
      const tamperedAttestation = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
        (value) => ({
          ...value,
          capture_attestation: {
            schema_version: 1,
            cell_status: "succeeded",
            secret_scan_path: null,
            secret_scan_sha256: null,
          },
        }),
      );
      assert.equal(await matrixCellRecoveryRecordFromArtifact(tamperedAttestation, "ubiquity/ai.ubq.fi"), null);
    } finally {
      for (const file of evidence.files) file.bytes.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "triage matrix evidence without a work selection derives the cell identity",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const evidence = await matrixCellEvidence(fixture, { workSelection: null });
    try {
      const recognized = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
      assert(recognized);
      assert.equal(recognized.identity.source_kind, "triage");
      assert.equal(recognized.identity.source_id, MATRIX_CELL_ID);
      assert.equal(recognized.identity.source_revision, evidence.record.identity.source_revision);
      // A foreign work selection never matches the embedded triage identity.
      const withSelection = matrixFilesWith(
        evidence.files,
        `reports/matrix/${MATRIX_CELL_ID}/recovery-record.json`,
        (value) => value,
      );
      withSelection.push({
        path: `reports/matrix/${MATRIX_CELL_ID}/work-selection.json`,
        bytes: textEncoder.encode(JSON.stringify({ schema_version: 1, ...MATRIX_WORK_SELECTION })),
      });
      assert.equal(await matrixCellRecoveryRecordFromArtifact(withSelection, "ubiquity/ai.ubq.fi"), null);
    } finally {
      for (const file of evidence.files) file.bytes.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix cell retry evidence recovers one deterministic quarantined candidate",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(17);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(7));
    try {
      const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
      assert(record);
      const result = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encrypted,
        keyBytes: key,
        record,
        artifactId: 9697049140,
      });
      assert.equal(result.disposition, "recovered");
      assert.equal(result.reason, "recovered");
      assert.equal(result.recovery_record?.phase, "checkpoint_durable");
      assert.equal(result.recovery_record?.candidate_sha, result.candidate_sha);
      assert.equal(result.changed_files[0], "candidate.txt");
      assert.match(
        result.candidate_branch ?? "",
        /^sentinel\/candidate-github_issue-208-[0-9a-f]{32}-g1-[0-9a-f]{16}$/u,
      );
      assert.equal(result.draft_pull_request?.body.draft, true);
      assert.equal(result.draft_pull_request?.body.base, "development");
      assert.equal(result.draft_pull_request?.auto_merge, false);
      assert.equal(JSON.stringify(result).includes(evidence.secret), false);
      const candidateSha = result.candidate_sha!;
      assert.equal(
        await git(fixture.checkout, ["show", `${candidateSha}:candidate.txt`]),
        "matrix recovered candidate",
      );
      assert.equal(
        await git(fixture.checkout, ["rev-list", "--parents", "-n", "1", candidateSha]),
        `${candidateSha} ${fixture.baseSha}`,
      );
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "matrix retry evidence is rejected before publication when any snapshot path leaves cell ownership",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const evidence = await matrixCellEvidence(fixture);
    const escaped = matrixFilesWith(
      evidence.files,
      `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
      (value) => ({
        ...value,
        files: [{ ...(value.files as Array<Record<string, unknown>>)[0]!, path: "outside.txt" }],
      }),
    );
    const escapedAsPackage = matrixFilesWith(
      evidence.files,
      `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
      (value) => ({
        ...value,
        files: [{ ...(value.files as Array<Record<string, unknown>>)[0]!, path: "candidate.txt" }],
        cell_contract: {
          ...(value.cell_contract as Record<string, unknown>),
          allowed_paths: [],
        },
      }),
    );
    try {
      const record = await matrixCellRecoveryRecordFromArtifact(escaped, "ubiquity/ai.ubq.fi");
      assert.equal(record, null);
      const recordEscaped = await matrixCellRecoveryRecordFromArtifact(escapedAsPackage, "ubiquity/ai.ubq.fi");
      assert.equal(recordEscaped, null);
    } finally {
      for (const file of evidence.files) file.bytes.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "stale-base matrix retry evidence never opens a draft from a historical base",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(17);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(7));
    try {
      const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
      assert(record);
      const result = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encrypted,
        keyBytes: key,
        record,
        expectedBaseSha: "1".repeat(40),
        artifactId: 9697049141,
      });
      assert.equal(result.disposition, "manual_required");
      assert.equal(result.reason, "artifact_wrong_base");
      assert.equal(result.candidate_branch, null);
      assert.equal(result.candidate_sha, null);
      assert.equal(result.draft_pull_request, null);
      assert.equal(JSON.stringify(result).includes(evidence.secret), false);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "later matrix evidence is retained under an artifact-scoped record when the work record is terminal",
  ignore: unavailable,
  fn() {
    const retention = matrixEvidenceRetentionRecord(
      parseSentinelRecoveryRecord({
        schema_version: 1,
        identity: {
          repository: "ubiquity/ai.ubq.fi",
          source_kind: "github_issue",
          source_id: "208",
          source_revision: "a".repeat(64),
          candidate_generation: 1,
        },
        run_id: "33197770000",
        attempt: 1,
        lease_token: "matrix-cell-33197770000-1-cell-credential",
        base_sha: "a".repeat(40),
        phase: "recovery_pending",
        disposition: "active",
        state_version: 1,
        created_at: "2026-08-28T19:00:00.000Z",
        updated_at: "2026-08-28T19:00:00.000Z",
        candidate_branch: null,
        candidate_sha: null,
        changed_files: [],
        tree_sha: null,
        failure_class: "transient_transport",
        failure_fingerprint: "8".repeat(64),
        artifact_ids: [],
        artifact_digests: [],
        reason: "reason",
        next_action: "next",
        predecessor: null,
      }),
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      makeArtifact(9697049142, "2026-08-28T20:00:00.000Z", { workflowRunId: 33197770000 }) as GitHubArtifact,
      `sha256:${"b".repeat(64)}`,
      `["terminal-key"]`,
    );
    assert.equal(retention?.identity.source_kind, "github_issue");
    assert.equal(retention?.identity.source_id, "208:artifact:9697049142");
    assert.equal(retention?.phase, "manual_required");
    assert.equal(retention?.failure_class, "unrecoverable_evidence");
    assert.equal(retention?.predecessor, `["terminal-key"]`);
  },
});

Deno.test({
  name: "matrix work-selection derivation reads the authenticated selection reports",
  ignore: unavailable,
  fn() {
    const issue = [
      {
        path: "reports/github-issue-selection.json",
        bytes: textEncoder.encode(JSON.stringify({
          schema_version: 1,
          issue_id: 208,
          issue_number: 208,
          fingerprint: "a".repeat(64),
        })),
      },
    ];
    const backlog = [
      {
        path: "reports/review-backlog-selection.json",
        bytes: textEncoder.encode(JSON.stringify({
          schema_version: 1,
          fingerprint: "b".repeat(64),
          affected_sha: "1".repeat(40),
        })),
      },
    ];
    try {
      assert.deepEqual(matrixCellWorkSelectionFromArtifact(issue), {
        source_kind: "github_issue",
        source_id: "208",
        source_revision: "a".repeat(64),
      });
      assert.deepEqual(matrixCellWorkSelectionFromArtifact(backlog), {
        source_kind: "review_backlog",
        source_id: "b".repeat(64),
        source_revision: "1".repeat(40),
      });
      assert.equal(matrixCellWorkSelectionFromArtifact([]), null);
    } finally {
      for (const file of [...issue, ...backlog]) file.bytes.fill(0);
    }
  },
});

Deno.test({
  name: "matrix recovery generation continues related records instead of resetting to one",
  ignore: unavailable,
  fn() {
    const base = {
      repository: "ubiquity/ai.ubq.fi",
      source_kind: "github_issue" as const,
      source_id: "208",
      source_revision: "9".repeat(64),
      candidate_generation: 3,
    };
    const unrelated = {
      ...base,
      source_revision: "8".repeat(64),
      candidate_generation: 7,
    };
    const records: SentinelRecoveryRecordV1[] = [
      parseSentinelRecoveryRecord({
        schema_version: 1,
        identity: base,
        run_id: "33197770000",
        attempt: 1,
        lease_token: "lease",
        base_sha: "a".repeat(40),
        phase: "rejected",
        disposition: "rejected",
        state_version: 1,
        created_at: "2026-08-28T19:00:00.000Z",
        updated_at: "2026-08-28T19:00:00.000Z",
        candidate_branch: null,
        candidate_sha: null,
        changed_files: [],
        tree_sha: null,
        failure_class: "unrecoverable_evidence",
        failure_fingerprint: "8".repeat(64),
        artifact_ids: [],
        artifact_digests: [],
        reason: "reason",
        next_action: null,
        predecessor: null,
      }),
      parseSentinelRecoveryRecord({
        schema_version: 1,
        identity: unrelated,
        run_id: "33197770000",
        attempt: 1,
        lease_token: "lease",
        base_sha: "a".repeat(40),
        phase: "rejected",
        disposition: "rejected",
        state_version: 1,
        created_at: "2026-08-28T19:00:00.000Z",
        updated_at: "2026-08-28T19:00:00.000Z",
        candidate_branch: null,
        candidate_sha: null,
        changed_files: [],
        tree_sha: null,
        failure_class: "unrecoverable_evidence",
        failure_fingerprint: "8".repeat(64),
        artifact_ids: [],
        artifact_digests: [],
        reason: "reason",
        next_action: null,
        predecessor: null,
      }),
    ];
    const current = { ...base, source_revision: "9".repeat(64) };
    assert.equal(resolveMatrixRecoveryGeneration(records, current), 4);
    const differentRevision = { ...base, source_revision: "7".repeat(64) };
    assert.equal(resolveMatrixRecoveryGeneration(records, differentRevision), 1);
  },
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
    const artifacts = Array.from({ length: 129 }, (_, index) =>
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
    assert.equal(selected.length, 128);
    assert.deepEqual(
      selected.map((artifact) => artifact.id),
      Array.from({ length: 128 }, (_, index) => 10_128 - index),
    );
    const withRequiredOldest = selectSentinelRecoveryArtifacts(artifacts, 128, new Set([10_000]));
    assert.equal(withRequiredOldest.length, 128);
    assert.equal(withRequiredOldest[0].id, 10_000);
    assert.equal(withRequiredOldest.some((artifact) => artifact.id === 10_001), false);
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
  assert.equal(record.identity.source_id, "33197180235:artifact:9697049137");
  assert.equal(record.identity.source_revision, headSha);
  assert.match(record.next_action ?? "", /repository owner/u);
});

Deno.test("authenticated unrecognized evidence receives a durable manual disposition", () => {
  const headSha = "d".repeat(40);
  const digest = `sha256:${"e".repeat(64)}`;
  const record = manualRecoveryRecordForLegacyArtifact(
    "ubiquity/ai.ubq.fi",
    makeArtifact(9697049138, "2026-08-28T18:25:53Z", {
      workflowRunId: 33197180236,
      workflowRunHeadSha: headSha,
    }),
    digest,
  );
  assert(record);
  assert.equal(record.failure_class, "artifact_invalid");
  assert.deepEqual(record.artifact_ids, [9697049138]);
  assert.deepEqual(record.artifact_digests, [digest]);
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

Deno.test("failed owning workflow terminalizes authenticated raw legacy evidence", () => {
  const files = [{ path: "raw-logs/private.log", bytes: new TextEncoder().encode("legacy evidence") }];
  try {
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "failure" }),
      true,
    );
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test("failed owning workflow terminalizes an authenticated empty legacy envelope", () => {
  assert.equal(
    legacyArtifactNeedsManualDisposition([], { status: "completed", conclusion: "failure" }),
    true,
  );
  assert.equal(
    legacyArtifactNeedsManualDisposition([], { status: "completed", conclusion: "success" }),
    false,
  );
});

Deno.test("report-only ciphertext can prove legacy failure without a workflow lookup", () => {
  const files = [
    { path: "reports/cycle.json", bytes: new TextEncoder().encode('{"schema_version":1,"status":"failed"}') },
  ];
  try {
    assert.equal(legacyArtifactNeedsManualDisposition(files), true);
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test("terminal report-only cycles are not reclassified from a later workflow failure", () => {
  const files = [
    { path: "reports/cycle.json", bytes: new TextEncoder().encode('{"schema_version":1,"status":"no_change"}') },
  ];
  try {
    assert.equal(
      legacyArtifactNeedsManualDisposition(files, { status: "completed", conclusion: "failure" }),
      false,
    );
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test("authenticated terminal legacy evidence maps to a durable terminal record", () => {
  const files = [
    {
      path: "reports/github-issue-disposition.json",
      bytes: new TextEncoder().encode('{"disposition":"manual_required"}'),
    },
  ];
  try {
    assert.equal(legacyArtifactTerminalDisposition(files), "manual_required");
    const record = terminalRecoveryRecordForLegacyArtifact(
      "ubiquity/ai.ubq.fi",
      makeArtifact(9697049137, "2026-08-28T18:25:53.000Z", {
        workflowRunId: 33197180235,
        workflowRunHeadSha: "a".repeat(40),
      }),
      `sha256:${"a".repeat(64)}`,
      "manual_required",
    );
    assert.equal(record?.phase, "manual_required");
    assert.equal(record?.disposition, "manual_required");
    assert.deepEqual(record?.artifact_ids, [9697049137]);
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
});

Deno.test("unrecognized authenticated terminal legacy evidence fails closed to manual review", () => {
  const files = [
    {
      path: "reports/github-issue-disposition.json",
      bytes: new TextEncoder().encode('{"legacy_disposition":"unknown"}'),
    },
  ];
  try {
    assert.equal(legacyArtifactTerminalDisposition(files), null);
    assert.equal(legacyArtifactHasTerminalReport(files), true);
    assert.equal(legacyArtifactNeedsManualDisposition(files), false);
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

const sha256Text = async (value: string): Promise<string> => await sha256(new TextEncoder().encode(value));

const fakeSha = async (value: string): Promise<string> => (await sha256Text(value)).slice(0, 40);

const createEvidenceZip = async (root: string, envelope: Uint8Array): Promise<Uint8Array<ArrayBuffer>> => {
  const source = `${root}/zip-src-${crypto.randomUUID()}`;
  const output = `${root}/evidence-${crypto.randomUUID()}.zip`;
  await Deno.mkdir(source, { recursive: true });
  await Deno.writeFile(`${source}/sentinel-evidence-v1.json`, envelope);
  try {
    const result = await new Deno.Command("zip", {
      args: ["-q", "-j", output, "sentinel-evidence-v1.json"],
      cwd: source,
    }).output();
    if (!result.success) throw new Error("zip fixture failed");
    return await Deno.readFile(output);
  } finally {
    await Deno.remove(source, { recursive: true }).catch(() => undefined);
    await Deno.remove(output).catch(() => undefined);
  }
};

const createBareOrigin = async (root: string, checkout: string): Promise<string> => {
  const bare = `${root}/origin.git`;
  await git(checkout, ["init", "--bare", bare]);
  await git(checkout, ["remote", "add", "origin", bare]);
  await git(checkout, ["push", "origin", "development"]);
  return bare;
};

type Deferred = Readonly<{ promise: Promise<void>; resolve: () => void; settled: boolean }>;

const deferred = (): Deferred => {
  let resolve!: () => void;
  let settled = false;
  const promise = new Promise<void>((done) => {
    resolve = () => {
      if (!settled) {
        settled = true;
        done();
      }
    };
  });
  return { promise, resolve, settled };
};

type FakeStoreArtifact = Readonly<{
  id: number;
  name: string;
  createdAt: string;
  zip: Uint8Array<ArrayBuffer>;
}>;

type FakeRecoveryStoreInput = Readonly<{
  devHead: string;
  artifacts: readonly FakeStoreArtifact[];
  branchShas?: Readonly<Map<string, string>>;
  initialLedger?: SentinelRecoveryLedgerV1;
  devHeadAfterReads?: Readonly<{ at: number; sha: string }>;
}>;

class FakeRecoveryStore {
  readonly repository = "ubiquity/ai.ubq.fi";
  readonly developmentHead: string;
  readonly artifacts: readonly FakeStoreArtifact[];
  readonly branchShas: ReadonlyMap<string, string>;
  readonly initialLedger: SentinelRecoveryLedgerV1;
  readonly devHeadAfterReads: Readonly<{ at: number; sha: string }> | null;
  currentStateCommit: string | null = null;
  readonly ledgerByCommit = new Map<string, unknown>();
  readonly commitParents = new Map<string, readonly string[]>();
  readonly pulls: Array<Record<string, unknown>> = [];
  nextPullNumber = 1;
  devReads = 0;
  stateReads = 0;
  refWrites = 0;
  pullCreates = 0;
  pullPatches = 0;
  private lastBlobJson: string | null = null;
  private race: { first: Deferred; second: Deferred; readsDone: boolean; firstDone: boolean } | null = null;

  constructor(input: FakeRecoveryStoreInput) {
    this.developmentHead = input.devHead;
    this.artifacts = input.artifacts;
    this.branchShas = input.branchShas ?? new Map();
    this.initialLedger = input.initialLedger ?? {
      schema_version: 1,
      records: [],
      retry_history: [],
      retry_decisions: [],
      leases: [],
    };
    this.devHeadAfterReads = input.devHeadAfterReads ?? null;
  }

  currentLedger(): SentinelRecoveryLedgerV1 {
    if (this.currentStateCommit !== null) {
      const value = this.ledgerByCommit.get(this.currentStateCommit);
      if (value !== undefined) return parseSentinelRecoveryLedger(value);
    }
    return parseSentinelRecoveryLedger(this.initialLedger);
  }

  enableConcurrencyRace(): void {
    this.race = { first: deferred(), second: deferred(), readsDone: false, firstDone: false };
  }

  private async holdRefWrite(): Promise<void> {
    if (this.race === null) return;
    if (this.refWrites === 1) await this.race.first.promise;
    if (this.refWrites === 2) await this.race.second.promise;
  }

  private releaseRefWrite(): void {
    if (this.race !== null && this.refWrites === 1) {
      this.race.firstDone = true;
      this.race.second.resolve();
    }
  }

  /** Fake GitHub Actions REST surface with a compare-and-swap state ref. */
  fetcher = async (input: string | URL | Request, init: RequestInit = {}): Promise<Response> => {
    const url = new URL(String(input));
    const path = url.pathname.replace(`/repos/${this.repository}`, "");
    const method = (init.method ?? "GET").toUpperCase();
    const fail = (status: number, message: string): Response => new Response(JSON.stringify({ message }), { status });
    const isJson = init.body === undefined || init.body === null
      ? method === "GET"
      : (init.headers as Record<string, string>)?.accept !== "application/octet-stream";
    const json = (value: unknown, status = 200): Response => new Response(JSON.stringify(value), { status });

    if (path === "/actions/artifacts") {
      return json({
        artifacts: this.artifacts.map((artifact) => ({
          id: artifact.id,
          name: artifact.name,
          size_in_bytes: artifact.zip.byteLength,
          expired: false,
          created_at: artifact.createdAt,
          expires_at: "2030-01-01T00:00:00.000Z",
          workflow_run: { id: 33197770000, head_sha: this.developmentHead },
        })),
      });
    }
    const zipMatch = path.match(/^\/actions\/artifacts\/([0-9]+)\/zip$/u);
    if (zipMatch) {
      const artifact = this.artifacts.find((candidate) => candidate.id === Number(zipMatch[1]));
      if (!artifact) return fail(404, "artifact missing");
      return new Response(artifact.zip, { status: 200 });
    }
    if (path === "/git/ref/heads/sentinel/recovery-state") {
      this.stateReads += 1;
      if (this.race !== null && !this.race.readsDone && this.stateReads >= 2) {
        this.race.readsDone = true;
        // Both jobs have read the same initial ledger; release every ref-write
        // gate so whichever write lands second observes the CAS conflict and
        // converges through the retry loop.
        this.race.first.resolve();
        this.race.second.resolve();
      }
      if (this.currentStateCommit === null) return fail(404, "state ref missing");
      return json({ ref: path, object: { sha: this.currentStateCommit, type: "commit" } });
    }
    if (path === "/git/ref/heads/development") {
      this.devReads += 1;
      const sha = this.devHeadAfterReads !== null && this.devReads >= this.devHeadAfterReads.at
        ? this.devHeadAfterReads.sha
        : this.developmentHead;
      // The live GitHub ref response names the exact ref and commit object.
      return json({ ref: "refs/heads/development", object: { sha, type: "commit" } });
    }
    const commitMatch = path.match(/^\/git\/commits\/([0-9a-f]{40})$/u);
    if (commitMatch) {
      return json({ sha: commitMatch[1], tree: { sha: await fakeSha(`tree:${commitMatch[1]}`) } });
    }
    const contentsMatch = path.match(/^\/contents\/(.+)$/u);
    if (contentsMatch) {
      const ref = url.searchParams.get("ref") ?? "";
      const value = this.ledgerByCommit.get(ref) ?? this.initialLedger;
      return json({ sha: ref, encoding: "base64", content: btoa(JSON.stringify(value)) });
    }
    if (path === "/git/blobs" && method === "POST") {
      const body = JSON.parse(String(init.body)) as { content?: unknown };
      this.lastBlobJson = typeof body.content === "string" ? body.content : null;
      if (this.lastBlobJson === null) return fail(400, "blob content missing");
      return json({ sha: await fakeSha(`blob:${this.lastBlobJson}`) });
    }
    if (path === "/git/trees" && method === "POST") {
      const body = String(init.body);
      return json({ sha: await fakeSha(`tree:${body}`) });
    }
    if (path === "/git/commits" && method === "POST") {
      const body = JSON.parse(String(init.body)) as {
        message?: unknown;
        tree?: unknown;
        parents?: unknown;
      };
      const parents = Array.isArray(body.parents) ? body.parents.map(String) : [];
      const sha = await fakeSha(`commit:${String(body.message)}:${String(body.tree)}:${parents.join(",")}`);
      if (this.lastBlobJson !== null) this.ledgerByCommit.set(sha, JSON.parse(this.lastBlobJson));
      this.commitParents.set(sha, parents);
      return json({ sha });
    }
    if (path === "/git/refs" && method === "POST") {
      const body = JSON.parse(String(init.body)) as { ref?: unknown; sha?: unknown };
      this.refWrites += 1;
      await this.holdRefWrite();
      if (this.currentStateCommit !== null) {
        this.releaseRefWrite();
        return fail(422, "GitHub recovery state POST /git/refs failed with HTTP 422");
      }
      this.currentStateCommit = String(body.sha);
      this.releaseRefWrite();
      return json({ ref: body.ref, object: { sha: body.sha, type: "commit" } });
    }
    if (path === "/git/refs/heads/sentinel/recovery-state" && method === "PATCH") {
      const body = JSON.parse(String(init.body)) as { sha?: unknown; force?: unknown };
      this.refWrites += 1;
      await this.holdRefWrite();
      const parents = this.commitParents.get(String(body.sha)) ?? [];
      if (this.currentStateCommit === null || !parents.includes(this.currentStateCommit)) {
        this.releaseRefWrite();
        return fail(422, "GitHub recovery state PATCH failed with HTTP 422");
      }
      this.currentStateCommit = String(body.sha);
      this.releaseRefWrite();
      return json({ ref: path, object: { sha: body.sha, type: "commit" } });
    }
    if (path === "/pulls" && method === "GET") return json(this.pulls);
    if (path === "/pulls" && method === "POST") {
      const body = JSON.parse(String(init.body)) as {
        head?: unknown;
        base?: unknown;
        body?: unknown;
        draft?: unknown;
      };
      // GitHub refuses a second open pull request for the same head+base; the
      // recovery runner must reuse the existing draft instead.
      if (this.pulls.some((candidate) => (candidate.head as Record<string, unknown>)?.ref === body.head)) {
        return fail(422, "A pull request already exists for this head branch");
      }
      const number = this.nextPullNumber;
      this.nextPullNumber += 1;
      this.pullCreates += 1;
      const headSha = this.branchShas.get(String(body.head)) ?? "0".repeat(40);
      const pull: Record<string, unknown> = {
        number,
        html_url: `https://github.com/${this.repository}/pull/${number}`,
        state: "open",
        merged_at: null,
        body: String(body.body ?? ""),
        head: { ref: body.head, sha: headSha },
        base: { ref: body.base },
        draft: body.draft === true,
        auto_merge: null,
      };
      this.pulls.push(pull);
      return json(pull, 201);
    }
    const pullPatch = path.match(/^\/pulls\/([0-9]+)$/u);
    if (pullPatch && method === "PATCH") {
      const body = JSON.parse(String(init.body)) as { body?: unknown; draft?: unknown };
      const pull = this.pulls.find((candidate) => candidate.number === Number(pullPatch[1]));
      if (!pull) return fail(404, "pull missing");
      this.pullPatches += 1;
      pull.body = String(body.body ?? "");
      pull.draft = body.draft === true;
      return json(pull);
    }
    void isJson;
    return fail(404, `unhandled fake route ${method} ${path}`);
  };
}

/**
 * Faithful GitHub REST surface: ref routes exist only under
 * `/repos/{owner}/{repo}`. The FakeRecoveryStore strips the repository prefix
 * and tolerates repository-less ref URLs, but the live API returns HTTP 404
 * for them — exactly the failure observed in the artifact-recovery run. This
 * wrapper enforces the live contract while recording every absolute URL.
 */
const liveContractFetcher = (
  store: FakeRecoveryStore,
): Readonly<{ fetcher: (input: string | URL | Request, init?: RequestInit) => Promise<Response>; urls: string[] }> => {
  const urls: string[] = [];
  const fetcher = async (input: string | URL | Request, init: RequestInit = {}): Promise<Response> => {
    const url = new URL(String(input));
    urls.push(url.href);
    const notFound = (): Response => new Response(JSON.stringify({ message: "Not Found" }), { status: 404 });
    if (url.origin !== "https://api.github.com" || !url.pathname.startsWith(`/repos/${store.repository}/`)) {
      return notFound();
    }
    return await store.fetcher(input, init);
  };
  return { fetcher, urls };
};

const encryptedArtifactKey = (key: Uint8Array): string => btoa(String.fromCharCode(...key));

const sentinelRepositoryEnvironment = (): Readonly<{ restore: () => void }> => {
  const previous = Deno.env.get("GITHUB_REPOSITORY");
  Deno.env.set("GITHUB_REPOSITORY", "ubiquity/ai.ubq.fi");
  return {
    restore: () => {
      if (previous === undefined) Deno.env.delete("GITHUB_REPOSITORY");
      else Deno.env.set("GITHUB_REPOSITORY", previous);
    },
  };
};

Deno.test({
  name:
    "identical matrix evidence replay resolves to the same record, branch, SHA, and draft PR without duplicate mutation",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(21);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(3));
    const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
    assert(record);
    const environment = await sentinelRepositoryEnvironment();
    try {
      const probe = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encrypted,
        keyBytes: key,
        record,
        artifactId: 9697049200,
      });
      assert.equal(probe.disposition, "recovered");
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049200,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
        branchShas: new Map([[probe.candidate_branch!, probe.candidate_sha!]]),
      });
      const invoke = () =>
        recoverSentinelArtifactsInActions({
          checkout: fixture.checkout,
          repository: "ubiquity/ai.ubq.fi",
          token: "fixture-token",
          encodedArtifactKey: encryptedArtifactKey(key),
          fetcher: store.fetcher,
        });
      const first = await invoke();
      assert.equal(first.length, 1);
      assert.equal(first[0]!.disposition, "recovered");
      assert.equal(first[0]!.candidate_sha, probe.candidate_sha);
      assert.equal(first[0]!.candidate_branch, probe.candidate_branch);
      const writesAfterFirst = store.refWrites;
      const createsAfterFirst = store.pullCreates;
      const patchesAfterFirst = store.pullPatches;
      const ledgerAfterFirst = parseSentinelRecoveryLedger(store.currentLedger());
      const childAfterFirst = ledgerAfterFirst.records.find((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision
      );
      assert(childAfterFirst);
      assert.equal(childAfterFirst.phase, "checkpoint_durable");
      assert.equal(childAfterFirst.candidate_sha, probe.candidate_sha);
      // The canonical parent linkage points at the exact child record.
      const parent = ledgerAfterFirst.records.find((candidate) =>
        candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208" &&
        candidate.identity.source_revision === MATRIX_WORK_SELECTION.source_revision
      );
      assert(parent);
      assert.equal(parent.candidate_sha, probe.candidate_sha);
      assert.equal(
        parent.predecessor,
        JSON.stringify([
          childAfterFirst.identity.repository,
          childAfterFirst.identity.source_kind,
          childAfterFirst.identity.source_id,
          childAfterFirst.identity.source_revision,
          childAfterFirst.identity.candidate_generation,
        ]),
      );

      const second = await invoke();
      assert.equal(second.length, 1);
      assert.equal(second[0]!.disposition, "recovered");
      assert.equal(second[0]!.reason, "recovered");
      assert.equal(second[0]!.candidate_sha, first[0]!.candidate_sha);
      assert.equal(second[0]!.candidate_branch, first[0]!.candidate_branch);
      assert.equal(second[0]!.tree_sha, first[0]!.tree_sha);
      // Exact replay performed no duplicate state mutation.
      assert.equal(store.refWrites, writesAfterFirst);
      assert.equal(store.pullCreates, createsAfterFirst);
      assert.equal(store.pullPatches, patchesAfterFirst);
      const ledgerAfterSecond = parseSentinelRecoveryLedger(store.currentLedger());
      const childAfterSecond = ledgerAfterSecond.records.find((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision
      );
      assert(childAfterSecond);
      assert.equal(childAfterSecond.state_version, childAfterFirst.state_version);
      assert.equal(childAfterSecond.updated_at, childAfterFirst.updated_at);

      // The next-cycle selection path sees the recovered checkpoint for the
      // issue and defers to it instead of claiming a fresh generation.
      const selection = resolveSentinelRecoverySelection({
        ledger: ledgerAfterSecond,
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "208",
        source_revision: MATRIX_WORK_SELECTION.source_revision,
        now: "2026-08-28T20:00:00.000Z",
      });
      assert.equal(selection.current_record?.phase, "checkpoint_durable");
      assert.equal(selection.current_record?.candidate_sha, probe.candidate_sha);
      assert.equal(selection.retry_is_due, false);
      // The canonical parent generation follows the exact work-item rule
      // (family maximum plus one), so a next-cycle selection never forks it.
      assert.equal(selection.current_record?.identity.candidate_generation, 2);
      assert.equal(selection.next_generation, 3);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "concurrent recovery of identical evidence resolves through ledger CAS without corruption or duplicate records",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(22);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(4));
    const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
    assert(record);
    const environment = await sentinelRepositoryEnvironment();
    try {
      const probe = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encrypted,
        keyBytes: key,
        record,
        artifactId: 9697049201,
      });
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049201,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
        branchShas: new Map([[probe.candidate_branch!, probe.candidate_sha!]]),
      });
      store.enableConcurrencyRace();
      const invoke = () =>
        recoverSentinelArtifactsInActions({
          checkout: fixture.checkout,
          repository: "ubiquity/ai.ubq.fi",
          token: "fixture-token",
          encodedArtifactKey: encryptedArtifactKey(key),
          fetcher: store.fetcher,
        });
      const [left, right] = await Promise.all([invoke(), invoke()]);
      for (const result of [left[0]!, right[0]!]) {
        assert.equal(result.disposition, "recovered");
        assert.equal(result.reason, "recovered");
        assert.notEqual(result.reason, "artifact_corrupt");
        assert.notEqual(result.reason, "ledger_conflict");
      }
      assert.equal(left[0]!.candidate_sha, right[0]!.candidate_sha);
      assert.equal(left[0]!.candidate_branch, right[0]!.candidate_branch);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const children = ledger.records.filter((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision ||
        candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208"
      );
      assert.equal(children.length, 2); // exactly one child plus one canonical parent
      const child = children.find((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision
      )!;
      assert.equal(child.phase, "checkpoint_durable");
      assert.equal(child.candidate_sha, probe.candidate_sha);
      // One successful CAS write plus one refused conflicting attempt; the
      // loser then re-read the ledger and reused the winner's record.
      assert.equal(store.refWrites, 2);
      assert.equal(store.pullCreates, 1);
      assert.equal(store.pulls.length, 1);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "recovery fails closed without publish or draft PR when development advances after reconstruction",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(23);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(5));
    const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
    assert(record);
    const environment = await sentinelRepositoryEnvironment();
    try {
      const advancedHead = "b".repeat(40);
      assert.notEqual(advancedHead, fixture.baseSha);
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        devHeadAfterReads: { at: 3, sha: advancedHead },
        artifacts: [{
          id: 9697049202,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results[0]!.disposition, "manual_required");
      assert.equal(results[0]!.reason, "development_head_advanced");
      assert.equal(results[0]!.candidate_branch, null);
      assert.equal(results[0]!.candidate_sha, null);
      assert.equal(results[0]!.draft_pull_request, null);
      assert.equal(results[0]!.recovery_record?.phase, "manual_required");
      assert.equal(results[0]!.recovery_record?.failure_class, "stale_source");
      assert.equal(store.pullCreates, 0);
      assert.equal(store.refWrites, 1);
      // The candidate branch never reached origin and no PR was opened.
      assert.doesNotMatch(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), /sentinel\/candidate-/u);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const child = ledger.records.find((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision
      );
      assert(child);
      assert.equal(child.phase, "manual_required");
      assert.equal(child.failure_class, "stale_source");
      // The stale manual disposition never linked a canonical parent on the
      // authoritative issue revision.
      assert.equal(
        ledger.records.some((candidate) =>
          candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208" &&
          candidate.identity.source_revision === MATRIX_WORK_SELECTION.source_revision
        ),
        false,
      );
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "repeated classified cell failures persist bounded retry history and open the terminal circuit",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(24);
    const environment = await sentinelRepositoryEnvironment();
    const artifacts: Array<{ evidence: MatrixFixture; encrypted: Uint8Array<ArrayBuffer> }> = [];
    try {
      const first = await matrixCellEvidence(fixture, { payloadText: "matrix recovered candidate one\n" });
      const second = await matrixCellEvidence(fixture, { payloadText: "matrix recovered candidate two\n" });
      const third = await matrixCellEvidence(fixture, { payloadText: "matrix recovered candidate three\n" });
      for (const evidence of [first, second, third]) {
        artifacts.push({
          evidence,
          encrypted: await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(6)),
        });
      }
      const record = await matrixCellRecoveryRecordFromArtifact(first.files, "ubiquity/ai.ubq.fi");
      assert(record);
      const probe = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: artifacts[0]!.encrypted,
        keyBytes: key,
        record,
        artifactId: 9697049210,
      });
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: await Promise.all(artifacts.map(async (artifact, index) => ({
          id: 9697049210 + index,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - (index + 1) * 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, artifact.encrypted),
        }))),
        branchShas: new Map([[probe.candidate_branch!, probe.candidate_sha!]]),
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.deepEqual(
        results.map((result) => result.disposition),
        ["recovered", "recovered", "manual_required"],
      );
      assert.equal(results[2]!.reason, "terminal_record");
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const childKey = JSON.stringify([
        "ubiquity/ai.ubq.fi",
        "github_issue",
        "208",
        record.identity.source_revision,
        1,
      ]);
      const child = ledger.records.find((candidate) =>
        JSON.stringify([
          candidate.identity.repository,
          candidate.identity.source_kind,
          candidate.identity.source_id,
          candidate.identity.source_revision,
          candidate.identity.candidate_generation,
        ]) === childKey
      );
      assert(child);
      assert.equal(child.phase, "manual_required");
      assert.equal(child.failure_class, "transient_transport");
      assert.equal(child.failure_fingerprint, record.failure_fingerprint);
      assert.equal(child.state_version, 3);
      const history = ledger.retry_history.filter((attempt) =>
        JSON.stringify([
          attempt.identity.repository,
          attempt.identity.source_kind,
          attempt.identity.source_id,
          attempt.identity.source_revision,
          attempt.identity.candidate_generation,
        ]) === childKey
      );
      assert.equal(history.length, 3);
      assert.deepEqual(history.map((attempt) => attempt.attempt), [1, 2, 3]);
      assert.ok(history.every((attempt) => attempt.failure_class === "transient_transport"));
      const decision = ledger.retry_decisions.find((entry) => entry.identity_key === childKey);
      assert(decision);
      assert.equal(decision.decision.circuit_open, true);
      assert.equal(decision.decision.identical_failure_count, 3);
      assert.equal(decision.decision.should_retry, false);
      assert.equal(decision.decision.disposition, "manual_required");
      // The durable checkpoint is never overwritten by later evidence.
      const parent = ledger.records.find((candidate) =>
        candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208" &&
        candidate.identity.source_revision === MATRIX_WORK_SELECTION.source_revision
      );
      assert(parent);
      assert.equal(parent.phase, "checkpoint_durable");
      assert.equal(parent.candidate_sha, probe.candidate_sha);
      assert.equal(parent.predecessor, childKey);
      assert.equal(store.pullPatches, 0);
      assert.equal(store.pullCreates, 1);
      // The later failure rounds did not demote or reopen the checkpoint:
      // the deterministic replay reused exactly one draft PR.
      assert.equal(store.pulls.length, 1);
      // The complete A/B/C artifact evidence is durable on the terminal child
      // and its linked canonical parent, so an exact replay of either
      // terminal-triggering artifact is matched before generation allocation.
      const digests = await Promise.all(
        artifacts.map(async (artifact) => `sha256:${await sha256(artifact.encrypted)}`),
      );
      const sortedDigests = [...digests].sort();
      const artifactIds = [9697049210, 9697049211, 9697049212];
      assert.deepEqual(child.artifact_ids, artifactIds);
      assert.deepEqual(child.artifact_digests, sortedDigests);
      assert.deepEqual(parent.artifact_ids, artifactIds);
      assert.deepEqual(parent.artifact_digests, sortedDigests);

      // Replay the exact terminal-triggering B and C artifacts through the
      // production entrypoint: the terminal generation/state, the bounded
      // retry history, and the circuit decision stay stable and no ledger,
      // branch, or pull-request mutation happens.
      const replayStore = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: await Promise.all(
          artifacts.slice(1).map(async (artifact, index) => ({
            id: 9697049211 + index,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
            createdAt: new Date(Date.now() - (index + 2) * 60_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, artifact.encrypted),
          })),
        ),
        branchShas: new Map([[probe.candidate_branch!, probe.candidate_sha!]]),
        initialLedger: ledger,
      });
      const remoteBefore = await git(fixture.checkout, ["ls-remote", "--heads", "origin"]);
      const replayResults = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: replayStore.fetcher,
      });
      assert.deepEqual(
        replayResults.map((result) => result.disposition),
        ["manual_required", "manual_required"],
      );
      for (const result of replayResults) {
        assert.equal(result.reason, "terminal_record");
        assert.equal(result.candidate_branch, null);
        assert.equal(result.candidate_sha, null);
        assert.equal(result.draft_pull_request, null);
        assert.equal(result.recovery_record?.phase, "manual_required");
        assert.equal(result.recovery_record?.disposition, "manual_required");
        assert.equal(result.recovery_record?.identity.candidate_generation, 1);
        assert.equal(result.recovery_record?.state_version, child.state_version);
      }
      assert.equal(replayStore.refWrites, 0);
      assert.equal(replayStore.pullCreates, 0);
      assert.equal(replayStore.pullPatches, 0);
      assert.equal(replayStore.pulls.length, 0);
      const afterReplay = parseSentinelRecoveryLedger(replayStore.currentLedger());
      assert.equal(afterReplay.records.length, 2);
      assert.equal(afterReplay.retry_history.length, 3);
      assert.deepEqual(afterReplay.retry_history.map((attempt) => attempt.attempt), [1, 2, 3]);
      assert.equal(afterReplay.retry_decisions.length, 1);
      assert.equal(afterReplay.retry_decisions[0]!.decision.circuit_open, true);
      assert.equal(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), remoteBefore);
    } finally {
      for (const artifact of artifacts) {
        artifact.encrypted.fill(0);
        for (const file of artifact.evidence.files) file.bytes.fill(0);
      }
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

const terminalEvidenceRecord = (
  record: SentinelRecoveryRecordV1,
  disposition: "manual_required" | "rejected" | "delivered",
  overrides: Partial<SentinelRecoveryRecordV1> = {},
): SentinelRecoveryRecordV1 =>
  parseSentinelRecoveryRecord({
    ...record,
    phase: disposition,
    disposition,
    candidate_branch: null,
    candidate_sha: null,
    tree_sha: null,
    changed_files: [],
    reason: `terminal ${disposition} disposition`,
    next_action: null,
    ...overrides,
  });

const openCircuitDecision = (
  identity: SentinelRecoveryRecordV1["identity"],
): Readonly<{ identity_key: string; decision: SentinelRetryDecision }> => ({
  identity_key: sentinelRecoveryIdentityKey(identity),
  decision: {
    disposition: "manual_required",
    should_retry: false,
    circuit_open: true,
    validation_repair_allowed: false,
    source_revision_changed: false,
    candidate_generation: identity.candidate_generation,
    attempt_count: 3,
    identical_failure_count: 3,
    backoff_ms: null,
    retry_at: null,
    failure_class: "transient_transport",
    failure_fingerprint: "8".repeat(64),
    reason: "The bounded retry circuit is open.",
    next_action: "A repository owner must review the durable candidate.",
  },
});

const legacyFailureEvidence = async (
  baseSha: string,
  payloadText = "legacy recovered candidate\n",
): Promise<SentinelArtifactFile[]> => {
  const payload = textEncoder.encode(payloadText);
  const manifest = {
    schema_version: 1,
    base_sha: baseSha,
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
  return [
    {
      path: "reports/cycle.json",
      bytes: textEncoder.encode(JSON.stringify({
        schema_version: 1,
        run_id: "33197610000",
        status: "failed",
        stage: "failed",
        started_at: "2026-08-28T18:00:00.000Z",
        base_development_sha: baseSha,
      })),
    },
    { path: "reports/failed-implementation-candidate/files/0000.bin", bytes: payload },
    {
      path: "reports/failed-implementation-candidate/manifest.json",
      bytes: textEncoder.encode(JSON.stringify(manifest)),
    },
  ];
};

Deno.test({
  name: "exact matrix artifact replay after every terminal disposition retains the terminal record with zero mutation",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(26);
    const environment = await sentinelRepositoryEnvironment();
    const encryptedStack: Uint8Array<ArrayBuffer>[] = [];
    const evidenceStack: SentinelArtifactFile[][] = [];
    try {
      const rows = [
        { disposition: "manual_required" as const, match: "id" as const, circuitOpen: false, canonical: false },
        { disposition: "rejected" as const, match: "digest" as const, circuitOpen: false, canonical: false },
        { disposition: "delivered" as const, match: "id" as const, circuitOpen: false, canonical: false },
        { disposition: "manual_required" as const, match: "digest" as const, circuitOpen: true, canonical: false },
        // A terminal canonical (work-item) record carries the same artifact
        // evidence through the parent linkage; replay must retain it too.
        { disposition: "delivered" as const, match: "id" as const, circuitOpen: false, canonical: true },
      ];
      for (const [rowIndex, row] of rows.entries()) {
        const evidence = await matrixCellEvidence(fixture);
        evidenceStack.push(evidence.files);
        const encrypted = await encryptSentinelArtifact(
          evidence.files,
          key,
          new Uint8Array(12).fill(9 + rowIndex),
        );
        encryptedStack.push(encrypted);
        const artifactId = 9697049500 + rowIndex;
        const digest = `sha256:${await sha256(encrypted)}`;
        const terminalIdentity = row.canonical
          ? {
            repository: "ubiquity/ai.ubq.fi",
            source_kind: "github_issue" as const,
            source_id: "208",
            source_revision: MATRIX_WORK_SELECTION.source_revision,
            candidate_generation: 2,
          }
          : evidence.record.identity;
        const terminalRecord = terminalEvidenceRecord(evidence.record, row.disposition, {
          identity: terminalIdentity,
          artifact_ids: row.match === "id" ? [artifactId] : [],
          artifact_digests: row.match === "digest" ? [digest] : [],
          updated_at: "2026-08-28T20:00:00.000Z",
        });
        const initialLedger: SentinelRecoveryLedgerV1 = {
          schema_version: 1,
          records: [terminalRecord],
          retry_history: [],
          retry_decisions: row.circuitOpen ? [openCircuitDecision(terminalIdentity)] : [],
          leases: [],
        };
        const store = new FakeRecoveryStore({
          devHead: fixture.baseSha,
          artifacts: [{
            id: artifactId,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
            createdAt: new Date(Date.now() - (rowIndex + 1) * 60_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encrypted),
          }],
          initialLedger,
        });
        const results = await recoverSentinelArtifactsInActions({
          checkout: fixture.checkout,
          repository: "ubiquity/ai.ubq.fi",
          token: "fixture-token",
          encodedArtifactKey: encryptedArtifactKey(key),
          fetcher: store.fetcher,
        });
        assert.equal(results.length, 1, row.disposition);
        assert.equal(results[0]!.disposition, "manual_required", row.disposition);
        assert.equal(results[0]!.reason, "terminal_record", row.disposition);
        assert.equal(results[0]!.candidate_branch, null, row.disposition);
        assert.equal(results[0]!.candidate_sha, null, row.disposition);
        assert.equal(results[0]!.draft_pull_request, null, row.disposition);
        // The exact existing terminal record is retained verbatim: same
        // disposition, generation, and state version.
        assert.equal(results[0]!.recovery_record?.phase, row.disposition, row.disposition);
        assert.equal(results[0]!.recovery_record?.disposition, row.disposition, row.disposition);
        assert.equal(
          results[0]!.recovery_record?.identity.candidate_generation,
          terminalIdentity.candidate_generation,
          row.disposition,
        );
        assert.equal(results[0]!.recovery_record?.state_version, terminalRecord.state_version, row.disposition);
        // Zero branch, PR, and ledger mutation for the exact replay.
        assert.equal(store.refWrites, 0, row.disposition);
        assert.equal(store.pullCreates, 0, row.disposition);
        assert.equal(store.pullPatches, 0, row.disposition);
        assert.doesNotMatch(
          await git(fixture.checkout, ["ls-remote", "--heads", "origin"]),
          /sentinel\/candidate-/u,
          row.disposition,
        );
        const after = parseSentinelRecoveryLedger(store.currentLedger());
        assert.equal(after.records.length, 1, row.disposition);
        assert.equal(after.records[0]!.state_version, terminalRecord.state_version, row.disposition);
        assert.equal(after.records[0]!.updated_at, terminalRecord.updated_at, row.disposition);
        assert.equal(after.retry_decisions.length, initialLedger.retry_decisions.length, row.disposition);
      }
    } finally {
      for (const value of encryptedStack) value.fill(0);
      for (const files of evidenceStack) for (const file of files) file.bytes.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "exact legacy artifact replay after every terminal disposition retains the terminal record with zero mutation",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(27);
    const environment = await sentinelRepositoryEnvironment();
    const encryptedStack: Uint8Array<ArrayBuffer>[] = [];
    const evidenceStack: SentinelArtifactFile[][] = [];
    try {
      const rows = [
        { disposition: "manual_required" as const, match: "id" as const, terminalReport: false },
        { disposition: "rejected" as const, match: "digest" as const, terminalReport: false },
        { disposition: "delivered" as const, match: "id" as const, terminalReport: false },
        // A `resolved` production disposition inside the ciphertext is a
        // delivered terminal; its exact replay also retains the record.
        { disposition: "delivered" as const, match: "id" as const, terminalReport: true },
      ];
      for (const [rowIndex, row] of rows.entries()) {
        const files = row.terminalReport
          ? [
            {
              path: "reports/github-issue-disposition.json",
              bytes: textEncoder.encode(JSON.stringify({ disposition: "resolved" })),
            },
          ]
          : await legacyFailureEvidence(fixture.baseSha, `legacy row ${rowIndex}\n`);
        evidenceStack.push(files);
        const encrypted = await encryptSentinelArtifact(
          files,
          key,
          new Uint8Array(12).fill(11 + rowIndex),
        );
        encryptedStack.push(encrypted);
        const artifactId = 9697049600 + rowIndex;
        const digest = `sha256:${await sha256(encrypted)}`;
        const identity = row.terminalReport
          ? {
            repository: "ubiquity/ai.ubq.fi",
            source_kind: "triage" as const,
            source_id: `33197770000:artifact:${artifactId}`,
            source_revision: fixture.baseSha,
            candidate_generation: 1,
          }
          : {
            repository: "ubiquity/ai.ubq.fi",
            source_kind: "triage" as const,
            source_id: "33197610000",
            source_revision: fixture.baseSha,
            candidate_generation: 1,
          };
        const terminalRecord = parseSentinelRecoveryRecord({
          schema_version: 1,
          identity,
          run_id: "33197770000",
          attempt: 1,
          lease_token: `artifact-${artifactId}`,
          base_sha: fixture.baseSha,
          phase: row.disposition,
          disposition: row.disposition,
          state_version: 1,
          created_at: "2026-08-28T18:00:00.000Z",
          updated_at: "2026-08-28T20:00:00.000Z",
          candidate_branch: null,
          candidate_sha: null,
          changed_files: [],
          tree_sha: null,
          failure_class: "unrecoverable_evidence",
          failure_fingerprint: "8".repeat(64),
          artifact_ids: row.match === "id" ? [artifactId] : [],
          artifact_digests: row.match === "digest" ? [digest] : [],
          reason: "terminal disposition reached",
          next_action: null,
          predecessor: null,
        });
        const store = new FakeRecoveryStore({
          devHead: fixture.baseSha,
          artifacts: [{
            id: artifactId,
            name: `sentinel-evidence-v1-${artifactId}`,
            createdAt: new Date(Date.now() - (rowIndex + 1) * 60_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encrypted),
          }],
          initialLedger: {
            schema_version: 1,
            records: [terminalRecord],
            retry_history: [],
            retry_decisions: [],
            leases: [],
          },
        });
        const results = await recoverSentinelArtifactsInActions({
          checkout: fixture.checkout,
          repository: "ubiquity/ai.ubq.fi",
          token: "fixture-token",
          encodedArtifactKey: encryptedArtifactKey(key),
          fetcher: store.fetcher,
        });
        assert.equal(results.length, 1, row.disposition);
        assert.equal(results[0]!.disposition, "manual_required", row.disposition);
        assert.equal(
          results[0]!.reason,
          row.terminalReport ? "artifact_invalid" : "terminal_record",
          row.disposition,
        );
        assert.equal(results[0]!.recovery_record?.disposition, row.disposition, row.disposition);
        assert.equal(
          results[0]!.recovery_record?.identity.candidate_generation,
          identity.candidate_generation,
          row.disposition,
        );
        assert.equal(results[0]!.recovery_record?.state_version, terminalRecord.state_version, row.disposition);
        assert.equal(store.refWrites, 0, row.disposition);
        assert.equal(store.pullCreates, 0, row.disposition);
        assert.equal(store.pullPatches, 0, row.disposition);
        assert.doesNotMatch(
          await git(fixture.checkout, ["ls-remote", "--heads", "origin"]),
          /sentinel\/candidate-/u,
          row.disposition,
        );
        const after = parseSentinelRecoveryLedger(store.currentLedger());
        assert.equal(after.records.length, 1, row.disposition);
        assert.equal(after.records[0]!.state_version, terminalRecord.state_version, row.disposition);
      }
    } finally {
      for (const value of encryptedStack) value.fill(0);
      for (const files of evidenceStack) for (const file of files) file.bytes.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "legacy recovery fails closed without branch publish when development advances before publication",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(28);
    const environment = await sentinelRepositoryEnvironment();
    const files = await legacyFailureEvidence(fixture.baseSha);
    const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(13));
    try {
      const advancedHead = "b".repeat(40);
      assert.notEqual(advancedHead, fixture.baseSha);
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        devHeadAfterReads: { at: 3, sha: advancedHead },
        artifacts: [{
          id: 9697049700,
          name: "sentinel-evidence-v1-9697049700",
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "manual_required");
      assert.equal(results[0]!.reason, "development_head_advanced");
      assert.equal(results[0]!.candidate_branch, null);
      assert.equal(results[0]!.candidate_sha, null);
      assert.equal(results[0]!.draft_pull_request, null);
      assert.equal(results[0]!.recovery_record?.phase, "manual_required");
      assert.equal(results[0]!.recovery_record?.failure_class, "stale_source");
      assert.equal(store.pullCreates, 0);
      assert.equal(store.pullPatches, 0);
      assert.equal(store.refWrites, 1);
      assert.doesNotMatch(
        await git(fixture.checkout, ["ls-remote", "--heads", "origin"]),
        /sentinel\/candidate-/u,
      );
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 1);
      assert.equal(ledger.records[0]!.phase, "manual_required");
      assert.equal(ledger.records[0]!.failure_class, "stale_source");
      assert.equal(ledger.records[0]!.candidate_sha, null);
      assert.equal(ledger.records[0]!.candidate_branch, null);
    } finally {
      for (const file of files) file.bytes.fill(0);
      encrypted.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "legacy recovery fails closed without draft PR when development advances after branch publication",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(29);
    const environment = await sentinelRepositoryEnvironment();
    const files = await legacyFailureEvidence(fixture.baseSha);
    const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(15));
    try {
      const advancedHead = "c".repeat(40);
      assert.notEqual(advancedHead, fixture.baseSha);
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        devHeadAfterReads: { at: 4, sha: advancedHead },
        artifacts: [{
          id: 9697049701,
          name: "sentinel-evidence-v1-9697049701",
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "manual_required");
      assert.equal(results[0]!.reason, "development_head_advanced");
      assert.equal(results[0]!.recovery_record?.phase, "manual_required");
      assert.equal(results[0]!.recovery_record?.failure_class, "stale_source");
      assert.equal(results[0]!.recovery_record?.candidate_branch, null);
      assert.equal(results[0]!.recovery_record?.candidate_sha, null);
      // The candidate branch reached origin before the second head check, but
      // no draft PR was opened after the head advanced.
      assert.match(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), /sentinel\/candidate-/u);
      assert.equal(store.pullCreates, 0);
      assert.equal(store.pullPatches, 0);
      assert.equal(store.pulls.length, 0);
      assert.equal(store.refWrites, 1);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 1);
      assert.equal(ledger.records[0]!.phase, "manual_required");
      assert.equal(ledger.records[0]!.failure_class, "stale_source");
    } finally {
      for (const file of files) file.bytes.fill(0);
      encrypted.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

const succeededMatrixCellReportFiles = async (
  fixture: Readonly<{ baseSha: string }>,
): Promise<SentinelArtifactFile[]> => {
  const unsigned = {
    schema_version: 1 as const,
    run_id: "33197770000",
    run_attempt: 1,
    plan_digest: "3".repeat(64),
    cell_id: MATRIX_CELL_ID,
    base_sha: fixture.baseSha,
    branch: `sentinel/candidate-33197770000-1-${MATRIX_CELL_ID}`,
    head_sha: "7".repeat(40),
    tree_sha: "7".repeat(40),
    changed_paths: ["candidate.txt"],
    finding_dispositions: [{
      finding_id: "fixture",
      fingerprint: "4".repeat(64),
      status: "implemented" as const,
      summary: "The bounded implementation cell completed the scoped repair.",
      changed_files: ["candidate.txt"],
      validation: ["fixture validation passed"],
    }],
    validation: {
      passed: true,
      checks: [{ name: "focused-validation", passed: true, detail: "focused cell validation passed" }],
    },
    replay: { attempted: false, passed: true, results: [] },
    status: "succeeded" as const,
    failure_reason: null,
    artifact_sha256: "7".repeat(64),
    report_digest: "0".repeat(64),
  };
  const report = { ...unsigned, report_digest: await matrixCellReportDigest(unsigned) };
  return [{ path: `reports/matrix/${MATRIX_CELL_ID}/cell.json`, bytes: textEncoder.encode(JSON.stringify(report)) }];
};

Deno.test({
  name: "authenticated succeeded matrix cell artifacts are skipped with zero ledger mutation and no classification",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(33);
    const environment = await sentinelRepositoryEnvironment();
    const files = await succeededMatrixCellReportFiles(fixture);
    try {
      const authenticated = await authenticatedMatrixCellReport(files);
      assert(authenticated);
      assert.equal(authenticated.status, "succeeded");
      const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(17));
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049800,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      // Zero result classification and zero ledger mutation.
      assert.deepEqual(results, []);
      assert.equal(store.refWrites, 0);
      assert.equal(store.pullCreates, 0);
      assert.equal(store.pullPatches, 0);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 0);
      assert.equal(ledger.retry_history.length, 0);
      assert.equal(ledger.retry_decisions.length, 0);
      assert.doesNotMatch(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), /sentinel\/candidate-/u);
    } finally {
      for (const file of files) file.bytes.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "malformed matrix retry artifacts still fail closed through legacy classification",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    const key = new Uint8Array(32).fill(34);
    const environment = await sentinelRepositoryEnvironment();
    const evidence = await matrixCellEvidence(fixture);
    try {
      // An authenticated retry_pending report without any recovery evidence
      // (no manifest, no recovery record) is not a normal terminal cell
      // report: it must stay owner-visible as artifact_invalid instead of
      // being silently skipped.
      const malformed = evidence.files.filter((file) =>
        file.path !== `reports/matrix/${MATRIX_CELL_ID}/recovery-record.json` &&
        file.path !== `reports/matrix/${MATRIX_CELL_ID}/manifest.json` &&
        file.path !== `reports/matrix/${MATRIX_CELL_ID}/files/0000.bin`
      );
      malformed.push({
        path: "reports/cycle.json",
        bytes: textEncoder.encode(JSON.stringify({
          schema_version: 1,
          run_id: "33197770000",
          status: "failed",
          stage: "failed",
          started_at: "2026-08-28T19:00:00.000Z",
          base_development_sha: fixture.baseSha,
        })),
      });
      const authenticated = await authenticatedMatrixCellReport(malformed);
      assert(authenticated);
      assert.equal(authenticated.status, "retry_pending");
      const encrypted = await encryptSentinelArtifact(malformed, key, new Uint8Array(12).fill(18));
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049801,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "manual_required");
      assert.equal(results[0]!.reason, "artifact_invalid");
      assert.equal(results[0]!.candidate_branch, null);
      assert.equal(results[0]!.draft_pull_request, null);
      assert.equal(store.refWrites, 1);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 1);
      assert.equal(ledger.records[0]!.disposition, "manual_required");
      assert.equal(ledger.records[0]!.failure_class, "artifact_invalid");
    } finally {
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retry_pending cell without a reconstructable candidate still schedules the bounded retry",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(36);
    const environment = await sentinelRepositoryEnvironment();
    // The cell failed transiently with no candidate edit, so its encrypted
    // snapshot is empty: recovery cannot reconstruct a checkpoint, but the
    // authenticated retry_pending evidence must still schedule the bounded
    // retry instead of terminalizing as rejected/no_candidate_diff.
    const evidence = await matrixCellEvidence(fixture, { changedPaths: [] });
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(21));
    try {
      assert.equal(
        (await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi"))?.phase,
        "recovery_pending",
      );
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049850,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "retry_pending");
      assert.equal(results[0]!.reason, "retry_scheduled");
      assert.equal(results[0]!.candidate_branch, null);
      assert.equal(results[0]!.candidate_sha, null);
      assert.equal(results[0]!.draft_pull_request, null);
      assert.equal(results[0]!.recovery_record?.phase, "retry_wait");
      assert.equal(store.pullCreates, 0);
      assert.equal(store.refWrites, 1);
      assert.doesNotMatch(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), /sentinel\/candidate-/u);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 1);
      const record = ledger.records[0]!;
      assert.equal(record.phase, "retry_wait");
      assert.equal(record.disposition, "active");
      assert.equal(record.failure_class, "transient_transport");
      assert.equal(record.failure_fingerprint, evidence.record.failure_fingerprint);
      assert.equal(record.state_version, 2);
      assert.deepEqual(record.changed_files, []);
      assert.deepEqual(record.artifact_ids, [9697049850]);
      const keyOf = (identity: SentinelRecoveryRecordV1["identity"]): string =>
        JSON.stringify([
          identity.repository,
          identity.source_kind,
          identity.source_id,
          identity.source_revision,
          identity.candidate_generation,
        ]);
      const childKey = keyOf(record.identity);
      const history = ledger.retry_history.filter((attempt) => keyOf(attempt.identity) === childKey);
      assert.equal(history.length, 1);
      assert.equal(history[0]!.attempt, 1);
      assert.equal(history[0]!.failure_class, "transient_transport");
      assert.equal(history[0]!.failure_fingerprint, evidence.record.failure_fingerprint);
      const decision = ledger.retry_decisions.find((entry) => entry.identity_key === childKey);
      assert(decision);
      assert.equal(decision.decision.disposition, "retry_wait");
      assert.equal(decision.decision.should_retry, true);
      assert.ok(decision.decision.retry_at !== null);
      assert.equal(decision.decision.circuit_open, false);
      // The retry record stays non-terminal: a later exact replay of the same
      // artifact keeps the scheduled retry and performs no new mutation.
      const replay = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(replay.length, 1);
      assert.equal(replay[0]!.disposition, "retry_pending");
      assert.equal(store.pullCreates, 0);
      assert.equal(store.refWrites, 1);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "invalid retry_pending evidence fails closed to manual_required instead of scheduling a retry",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(37);
    const environment = await sentinelRepositoryEnvironment();
    // The report, recovery record, and manifest bind exactly, but the
    // candidate payload does not match its authenticated digest: the snapshot
    // is invalid evidence. The bounded retry circuit would allow a retry (the
    // original failure is transient), yet recovery must fail closed to
    // manual_required instead of scheduling one from tampered evidence.
    const evidence = await matrixCellEvidence(fixture);
    const tampered = matrixFilesWith(
      evidence.files,
      `reports/matrix/${MATRIX_CELL_ID}/manifest.json`,
      (value) => ({
        ...value,
        files: [{ ...(value.files as Array<Record<string, unknown>>)[0]!, sha256: "0".repeat(64) }],
      }),
    );
    const encrypted = await encryptSentinelArtifact(tampered, key, new Uint8Array(12).fill(22));
    try {
      assert.equal(
        (await matrixCellRecoveryRecordFromArtifact(tampered, "ubiquity/ai.ubq.fi"))?.phase,
        "recovery_pending",
      );
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049851,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "manual_required");
      assert.equal(results[0]!.reason, "artifact_invalid");
      assert.equal(results[0]!.candidate_branch, null);
      assert.equal(results[0]!.candidate_sha, null);
      assert.equal(results[0]!.draft_pull_request, null);
      assert.equal(JSON.stringify(results).includes(evidence.secret), false);
      assert.equal(store.pullCreates, 0);
      assert.equal(store.refWrites, 1);
      assert.doesNotMatch(await git(fixture.checkout, ["ls-remote", "--heads", "origin"]), /sentinel\/candidate-/u);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      assert.equal(ledger.records.length, 1);
      const record = ledger.records[0]!;
      assert.equal(record.phase, "manual_required");
      assert.equal(record.disposition, "manual_required");
      assert.equal(record.candidate_branch, null);
      assert.equal(record.candidate_sha, null);
      assert.equal(record.artifact_digests.length, 1);
      assert.equal(ledger.retry_history.length, 0);
      assert.equal(ledger.retry_decisions.length, 0);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of tampered) file.bytes.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

const MATRIX_CELL_B_ID = `cell-${"8".repeat(64)}`;
const MATRIX_CELL_B_OPTIONS = {
  cellId: MATRIX_CELL_B_ID,
  findingId: "fixture-b",
  findingFingerprint: "5".repeat(64),
  allowedPath: "candidate-b.txt",
};

Deno.test({
  name: "two interrupted cells for one issue reuse a single canonical parent and merge both checkpoints' evidence",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(35);
    const environment = await sentinelRepositoryEnvironment();
    const evidenceA = await matrixCellEvidence(fixture, { payloadText: "cell A candidate\n" });
    const evidenceB = await matrixCellEvidence(fixture, {
      payloadText: "cell B candidate\n",
      ...MATRIX_CELL_B_OPTIONS,
    });
    const encryptedA = await encryptSentinelArtifact(evidenceA.files, key, new Uint8Array(12).fill(19));
    const encryptedB = await encryptSentinelArtifact(evidenceB.files, key, new Uint8Array(12).fill(20));
    const encryptedStack = [encryptedA, encryptedB];
    try {
      const recordA = await matrixCellRecoveryRecordFromArtifact(evidenceA.files, "ubiquity/ai.ubq.fi");
      const recordB = await matrixCellRecoveryRecordFromArtifact(evidenceB.files, "ubiquity/ai.ubq.fi");
      assert(recordA);
      assert(recordB);
      assert.notEqual(recordA.identity.source_revision, recordB.identity.source_revision);
      const probeA = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encryptedA,
        keyBytes: key,
        record: recordA,
        artifactId: 9697049810,
      });
      const probeB = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encryptedB,
        keyBytes: key,
        record: recordB,
        artifactId: 9697049811,
      });
      assert.equal(probeA.disposition, "recovered");
      assert.equal(probeB.disposition, "recovered");
      assert.notEqual(probeA.candidate_sha, probeB.candidate_sha);
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [
          {
            id: 9697049810,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
            createdAt: new Date(Date.now() - 60_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encryptedA),
          },
          {
            id: 9697049811,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_B_ID}`,
            createdAt: new Date(Date.now() - 120_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encryptedB),
          },
        ],
        branchShas: new Map([
          [probeA.candidate_branch!, probeA.candidate_sha!],
          [probeB.candidate_branch!, probeB.candidate_sha!],
        ]),
      });
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: store.fetcher,
      });
      assert.deepEqual(results.map((result) => result.disposition), ["recovered", "recovered"]);
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const issueFamily = ledger.records.filter((candidate) =>
        candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208"
      );
      assert.equal(issueFamily.length, 3); // one child per cell plus exactly one canonical parent
      const parents = issueFamily.filter((candidate) =>
        candidate.identity.source_revision === MATRIX_WORK_SELECTION.source_revision
      );
      assert.equal(parents.length, 1);
      const parent = parents[0]!;
      assert.equal(parent.phase, "checkpoint_durable");
      assert.equal(parent.candidate_sha, probeA.candidate_sha);
      assert.equal(
        parent.predecessor,
        JSON.stringify([
          recordA.identity.repository,
          recordA.identity.source_kind,
          recordA.identity.source_id,
          recordA.identity.source_revision,
          recordA.identity.candidate_generation,
        ]),
      );
      const digests = (await Promise.all([encryptedA, encryptedB].map(async (encrypted) =>
        `sha256:${await sha256(encrypted)}`
      )))
        .sort();
      assert.deepEqual(parent.artifact_ids, [9697049810, 9697049811]);
      assert.deepEqual(parent.artifact_digests, digests);
      const childA = issueFamily.find((candidate) =>
        candidate.identity.source_revision === recordA.identity.source_revision
      )!;
      const childB = issueFamily.find((candidate) =>
        candidate.identity.source_revision === recordB.identity.source_revision
      )!;
      assert.equal(childA.phase, "checkpoint_durable");
      assert.equal(childA.candidate_sha, probeA.candidate_sha);
      assert.equal(childB.phase, "checkpoint_durable");
      assert.equal(childB.candidate_sha, probeB.candidate_sha);
      // Neither child's evidence is orphaned: each carries its own artifact.
      assert.deepEqual(childA.artifact_ids, [9697049810]);
      assert.deepEqual(childB.artifact_ids, [9697049811]);
      // The next-cycle selection deterministically sees exactly one parent.
      const selection = resolveSentinelRecoverySelection({
        ledger,
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "208",
        source_revision: MATRIX_WORK_SELECTION.source_revision,
        now: "2026-08-28T20:00:00.000Z",
      });
      assert.equal(selection.related_records.length, 3);
      assert.equal(selection.current_record?.identity.candidate_generation, parent.identity.candidate_generation);
      assert.equal(selection.current_record?.candidate_sha, probeA.candidate_sha);
      assert.equal(selection.current_record?.phase, "checkpoint_durable");
      assert.equal(selection.retry_is_due, false);
      assert.equal(selection.next_generation, parent.identity.candidate_generation + 1);
    } finally {
      for (const encrypted of encryptedStack) encrypted.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "concurrent recovery of two cells for one issue converges on one canonical parent through ledger CAS",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const key = new Uint8Array(32).fill(36);
    const environment = await sentinelRepositoryEnvironment();
    const evidenceA = await matrixCellEvidence(fixture, { payloadText: "cell A candidate\n" });
    const evidenceB = await matrixCellEvidence(fixture, {
      payloadText: "cell B candidate\n",
      ...MATRIX_CELL_B_OPTIONS,
    });
    const recordA = await matrixCellRecoveryRecordFromArtifact(evidenceA.files, "ubiquity/ai.ubq.fi");
    const recordB = await matrixCellRecoveryRecordFromArtifact(evidenceB.files, "ubiquity/ai.ubq.fi");
    assert(recordA);
    assert(recordB);
    const encryptedA = await encryptSentinelArtifact(evidenceA.files, key, new Uint8Array(12).fill(21));
    const encryptedB = await encryptSentinelArtifact(evidenceB.files, key, new Uint8Array(12).fill(22));
    const encryptedStack = [encryptedA, encryptedB];
    try {
      const probeA = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encryptedA,
        keyBytes: key,
        record: recordA,
        artifactId: 9697049820,
      });
      const probeB = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encryptedB,
        keyBytes: key,
        record: recordB,
        artifactId: 9697049821,
      });
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [
          {
            id: 9697049820,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
            createdAt: new Date(Date.now() - 60_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encryptedA),
          },
          {
            id: 9697049821,
            name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_B_ID}`,
            createdAt: new Date(Date.now() - 120_000).toISOString(),
            zip: await createEvidenceZip(fixture.root, encryptedB),
          },
        ],
        branchShas: new Map([
          [probeA.candidate_branch!, probeA.candidate_sha!],
          [probeB.candidate_branch!, probeB.candidate_sha!],
        ]),
      });
      store.enableConcurrencyRace();
      const invoke = () =>
        recoverSentinelArtifactsInActions({
          checkout: fixture.checkout,
          repository: "ubiquity/ai.ubq.fi",
          token: "fixture-token",
          encodedArtifactKey: encryptedArtifactKey(key),
          fetcher: store.fetcher,
        });
      const [left, right] = await Promise.all([invoke(), invoke()]);
      for (const result of [...left, ...right]) {
        assert.equal(result.disposition, "recovered");
        assert.notEqual(result.reason, "ledger_conflict");
        assert.notEqual(result.reason, "artifact_corrupt");
      }
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const issueFamily = ledger.records.filter((candidate) =>
        candidate.identity.source_kind === "github_issue" && candidate.identity.source_id === "208"
      );
      assert.equal(issueFamily.length, 3);
      const parents = issueFamily.filter((candidate) =>
        candidate.identity.source_revision === MATRIX_WORK_SELECTION.source_revision
      );
      assert.equal(parents.length, 1);
      const parent = parents[0]!;
      const digests = (await Promise.all([encryptedA, encryptedB].map(async (encrypted) =>
        `sha256:${await sha256(encrypted)}`
      )))
        .sort();
      assert.deepEqual(parent.artifact_ids, [9697049820, 9697049821]);
      assert.deepEqual(parent.artifact_digests, digests);
      const childA = issueFamily.find((candidate) =>
        candidate.identity.source_revision === recordA.identity.source_revision
      )!;
      const childB = issueFamily.find((candidate) =>
        candidate.identity.source_revision === recordB.identity.source_revision
      )!;
      assert.equal(childA.phase, "checkpoint_durable");
      assert.equal(childB.phase, "checkpoint_durable");
      assert.equal(childA.artifact_ids.length, 1);
      assert.equal(childB.artifact_ids.length, 1);
      const selection = resolveSentinelRecoverySelection({
        ledger,
        repository: "ubiquity/ai.ubq.fi",
        source_kind: "github_issue",
        source_id: "208",
        source_revision: MATRIX_WORK_SELECTION.source_revision,
        now: "2026-08-28T20:00:00.000Z",
      });
      assert.equal(selection.related_records.length, 3);
      assert.equal(selection.current_record?.identity.candidate_generation, parent.identity.candidate_generation);
      assert.equal(selection.retry_is_due, false);
      assert.equal(selection.next_generation, parent.identity.candidate_generation + 1);
    } finally {
      for (const encrypted of encryptedStack) encrypted.fill(0);
      key.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test("recovery development head read uses the exact repository-scoped ref contract", async () => {
  const repository = "ubiquity/ai.ubq.fi";
  const developmentSha = "3f3d8c46b389dee58489aac1b88d4a01972d9e94";
  const requested: string[] = [];
  // Mirrors the live `GET /repos/{owner}/{repo}/git/ref/heads/development`
  // response: an exact ref name, a commit object, and a full SHA. Every other
  // route — including the repository-less `/git/ref/...` form the recovery
  // runner used to request — returns the live HTTP 404.
  const liveGitHub = (input: string | URL | Request): Promise<Response> => {
    const url = new URL(String(input));
    requested.push(url.href);
    const fail = (status: number, message: string): Response => new Response(JSON.stringify({ message }), { status });
    if (!url.pathname.startsWith(`/repos/${repository}/`)) return Promise.resolve(fail(404, "Not Found"));
    if (url.pathname !== `/repos/${repository}/git/ref/heads/development`) {
      return Promise.resolve(fail(404, `unexpected route ${url.pathname}`));
    }
    return Promise.resolve(
      new Response(
        JSON.stringify({
          ref: "refs/heads/development",
          object: { sha: developmentSha, type: "commit" },
        }),
        { status: 200 },
      ),
    );
  };
  const head = await currentRecoveryDevelopmentHead("fixture-token", repository, liveGitHub);
  assert.equal(head, developmentSha);
  assert.deepEqual(requested, [`https://api.github.com/repos/${repository}/git/ref/heads/development`]);
  // A repository-less ref request is the exact live failure: HTTP 404 surfaces
  // and nothing is guessed or salvaged.
  await assert.rejects(
    () =>
      currentRecoveryDevelopmentHead(
        "fixture-token",
        repository,
        () => Promise.resolve(new Response(JSON.stringify({ message: "Not Found" }), { status: 404 })),
      ),
    /HTTP 404/,
  );
  // Every identity mismatch stays fail-closed: wrong ref, non-commit object,
  // and non-full SHA are all rejected.
  await assert.rejects(
    () =>
      currentRecoveryDevelopmentHead("fixture-token", repository, () =>
        Promise.resolve(
          new Response(
            JSON.stringify({
              ref: "refs/heads/main",
              object: { sha: developmentSha, type: "commit" },
            }),
            { status: 200 },
          ),
        )),
    /development head is invalid/,
  );
  await assert.rejects(
    () =>
      currentRecoveryDevelopmentHead("fixture-token", repository, () =>
        Promise.resolve(
          new Response(
            JSON.stringify({
              ref: "refs/heads/development",
              object: { sha: developmentSha, type: "tag" },
            }),
            { status: 200 },
          ),
        )),
    /development head is invalid/,
  );
  await assert.rejects(
    () =>
      currentRecoveryDevelopmentHead("fixture-token", repository, () =>
        Promise.resolve(
          new Response(
            JSON.stringify({
              ref: "refs/heads/development",
              object: { sha: "abcdef", type: "commit" },
            }),
            { status: 200 },
          ),
        )),
    /development head is invalid/,
  );
});

Deno.test({
  name: "recovery flow reads the exact development head through the repository-scoped ref URL",
  ignore: unavailable,
  async fn() {
    const fixture = await createBaseCheckout();
    await createBareOrigin(fixture.root, fixture.checkout);
    const evidence = await matrixCellEvidence(fixture);
    const key = new Uint8Array(32).fill(27);
    const encrypted = await encryptSentinelArtifact(evidence.files, key, new Uint8Array(12).fill(11));
    const record = await matrixCellRecoveryRecordFromArtifact(evidence.files, "ubiquity/ai.ubq.fi");
    assert(record);
    const environment = await sentinelRepositoryEnvironment();
    try {
      const probe = await recoverSentinelArtifactCandidate({
        checkout: fixture.checkout,
        encryptedBytes: encrypted,
        keyBytes: key,
        record,
        artifactId: 9697049300,
      });
      assert.equal(probe.disposition, "recovered");
      const store = new FakeRecoveryStore({
        devHead: fixture.baseSha,
        artifacts: [{
          id: 9697049300,
          name: `sentinel-matrix-cell-v1-33197770000-1-${MATRIX_CELL_ID}`,
          createdAt: new Date(Date.now() - 60_000).toISOString(),
          zip: await createEvidenceZip(fixture.root, encrypted),
        }],
        branchShas: new Map([[probe.candidate_branch!, probe.candidate_sha!]]),
      });
      const live = liveContractFetcher(store);
      const results = await recoverSentinelArtifactsInActions({
        checkout: fixture.checkout,
        repository: "ubiquity/ai.ubq.fi",
        token: "fixture-token",
        encodedArtifactKey: encryptedArtifactKey(key),
        fetcher: live.fetcher,
      });
      assert.equal(results.length, 1);
      assert.equal(results[0]!.disposition, "recovered");
      assert.equal(results[0]!.candidate_sha, probe.candidate_sha);
      const developmentRef = `https://api.github.com/repos/${store.repository}/git/ref/heads/development`;
      assert.ok(
        live.urls.some((url) => url === developmentRef),
        "the exact repository-scoped development ref URL must be requested",
      );
      assert.ok(
        !live.urls.some((url) => url === "https://api.github.com/git/ref/heads/development"),
        "the repository-less ref URL that live-404s must never be requested",
      );
      const ledger = parseSentinelRecoveryLedger(store.currentLedger());
      const child = ledger.records.find((candidate) =>
        candidate.identity.source_revision === evidence.record.identity.source_revision
      );
      assert(child);
      assert.equal(child.candidate_sha, probe.candidate_sha);
      assert.equal(child.base_sha, fixture.baseSha);
    } finally {
      encrypted.fill(0);
      key.fill(0);
      for (const file of evidence.files) file.bytes.fill(0);
      environment.restore();
      await Deno.remove(fixture.root, { recursive: true });
    }
  },
});

Deno.test({
  name: "authenticated retained issue plan recovery reuses frozen V2 evidence without a planner",
  ignore: unavailable,
  async fn() {
    const key = new Uint8Array(32).fill(11);
    const keyB64 = encryptedArtifactKey(key);
    const repository = "ubiquity/ai.ubq.fi";
    const baseSha = "1".repeat(40);
    const candidateSha = "2".repeat(40);
    const job = {
      repository,
      issueId: 5228586364,
      nodeId: "I_kwDOQoe6nc8AAAABN6X112",
      number: 112,
      htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/112",
      title: "Retained issue",
      body: "Ordinary retained source body.",
      bodySha256: "a".repeat(64),
      fingerprint: "3".repeat(64),
      priority: "P3" as const,
      priorityLabel: "Priority: 2 (Medium)",
      queuePriority: null,
      queuePriorityAmbiguous: false,
      timeLabel: null,
      intake: "backlog" as const,
      labels: [],
      files: [] as string[],
      acceptance: [] as string[],
      materialDigest: null,
      capturedComments: [] as unknown[],
      authorLogin: "0x4007",
      authorAssociation: "MEMBER",
      comments: 0,
      createdAt: "2026-09-01T00:00:00Z",
      updatedAt: "2026-09-01T00:01:00Z",
      relations: {
        parentIssueNumber: null,
        subIssueCount: 0,
        blockedByCount: 0,
        blockingCount: 0,
        latestBodyEdit: null,
        latestTitleEdit: null,
      },
    } as unknown as Parameters<typeof loadRetainedIssueFrozenPlan>[0]["job"];
    const interval = {
      schema_version: 1,
      mode: "hourly",
      start: "2026-09-01T00:00:00.000Z",
      end: "2026-09-01T01:00:00.000Z",
      duration_ms: 60 * 60_000,
    };
    const triage = {
      schema_version: 1,
      interval,
      findings: [{
        id: issueJobFindingId(job),
        fingerprint: job.fingerprint,
        severity: "P3" as const,
        title: job.title,
        affected_surface: "src/admin.ts",
        allowed_paths: ["src/admin.ts", "tests/"],
        shared_paths: [],
        depends_on: [],
        evidence: [{ source: "github_issue", reference: job.htmlUrl, detail: job.body }],
        proposed_correction: "Implement the retained issue.",
        validation_requirements: ["Run deno fmt and affected tests"],
        actionable: true,
      }],
      no_findings_reason: null,
    };
    const planSha256 = await githubIssuePlanDigest({
      repository,
      issue_id: job.issueId,
      fingerprint: job.fingerprint,
      base_sha: baseSha,
      plan: triage as never,
    });
    const cycle = {
      schema_version: 1,
      run_id: "123456789",
      started_at: "2026-09-01T00:00:00.000Z",
      base_development_sha: baseSha,
    };
    const selection = {
      schema_version: 2 as const,
      issue_id: job.issueId,
      issue_number: job.number,
      fingerprint: job.fingerprint,
      body_sha256: job.bodySha256,
      comments: 0,
      priority: "P3" as const,
      time_label: null,
      files: [] as string[],
      updated_at: job.updatedAt,
      base_sha: baseSha,
      plan_sha256: planSha256,
    };
    const record = parseSentinelRecoveryRecord({
      schema_version: 1,
      identity: {
        repository,
        source_kind: "github_issue",
        source_id: String(job.issueId),
        source_revision: job.fingerprint,
        candidate_generation: 1,
      },
      run_id: "123456789",
      attempt: 1,
      lease_token: "lease",
      base_sha: baseSha,
      phase: "checkpoint_durable",
      disposition: "active",
      state_version: 4,
      created_at: "2026-09-01T00:00:00.000Z",
      updated_at: "2026-09-01T00:10:00.000Z",
      candidate_branch: "sentinel/candidate-123456789-1",
      candidate_sha: candidateSha,
      changed_files: ["src/admin.ts"],
      tree_sha: "4".repeat(40),
      failure_class: null,
      failure_fingerprint: null,
      artifact_ids: [9697049201],
      artifact_digests: [],
      reason: "retained",
      next_action: "resume",
      predecessor: null,
    });
    const checkpoint = { branch: "sentinel/candidate-123456789-1", sha: candidateSha, baseSha };
    const root = await Deno.makeTempDir({ prefix: "sentinel-retained-loader-" });
    try {
      const textEncoder = new TextEncoder();
      const retryReport = {
        schema_version: 1,
        issue_id: job.issueId,
        issue_number: job.number,
        fingerprint: job.fingerprint,
        phase: "failed_implementation",
        implementation_status: "blocked",
        disposition: "retry_pending",
        retry_checkpoint: {
          branch: checkpoint.branch,
          sha: checkpoint.sha,
          base_sha: checkpoint.baseSha,
        },
      };
      const files: SentinelArtifactFile[] = [
        { path: "reports/cycle.json", bytes: textEncoder.encode(JSON.stringify(cycle)) },
        { path: "reports/github-issue-selection.json", bytes: textEncoder.encode(JSON.stringify(selection)) },
        { path: "reports/triage.json", bytes: textEncoder.encode(JSON.stringify(triage)) },
        { path: "reports/recovery-record-v1.json", bytes: textEncoder.encode(JSON.stringify(record)) },
        { path: "reports/github-issue-disposition.json", bytes: textEncoder.encode(JSON.stringify(retryReport)) },
      ];
      const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(2));
      const zip = await createEvidenceZip(root, encrypted);
      const envelope = await decryptSentinelEvidenceEnvelope({ zip, encodedArtifactKey: keyB64, privateRoot: root });
      assert.ok(envelope.files.length === 5);
      const withDigest = { ...record, artifact_digests: [envelope.encrypted_digest] };
      const download = (): Promise<Uint8Array> => Promise.resolve(zip);

      const artifacts = [{
        workflow_run_id: 123456789,
        expired: false,
        expires_at: null,
        size_in_bytes: 1_024,
      }];
      const loaded = await loadRetainedIssueFrozenPlan({
        repository,
        job,
        checkpoint,
        record: withDigest,
        artifacts,
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      assert.equal(loaded.available, true);
      if (loaded.available) {
        assert.equal(loaded.triage.findings[0]!.fingerprint, job.fingerprint);
        assert.deepEqual(loaded.triage.findings[0]!.allowed_paths, ["src/admin.ts", "tests/"]);
      }
      // Wrong digest: explicit unavailable, checkpoint preserved.
      const wrongDigest = await loadRetainedIssueFrozenPlan({
        repository,
        job,
        checkpoint,
        record: { ...record, artifact_digests: [`sha256:${"0".repeat(64)}`] },
        artifacts,
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      assert.equal(wrongDigest.available, false);
      if (!wrongDigest.available) {
        assert.equal(wrongDigest.reason.includes("digest"), true);
      }
      // Wrong checkpoint base: unavailable.
      const wrongBase = await loadRetainedIssueFrozenPlan({
        repository,
        job,
        checkpoint: { ...checkpoint, baseSha: "5".repeat(40) },
        record: withDigest,
        artifacts,
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      assert.equal(wrongBase.available, false);
      // Wrong checkpoint branch: unavailable.
      const wrongBranch = await loadRetainedIssueFrozenPlan({
        repository,
        job,
        checkpoint: { ...checkpoint, branch: "sentinel/candidate-999999999-1" },
        record: withDigest,
        artifacts,
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      assert.equal(wrongBranch.available, false);
      // Wrong source fingerprint: unavailable.
      const wrongSource = await loadRetainedIssueFrozenPlan({
        repository,
        job: { ...job, fingerprint: "6".repeat(64) },
        checkpoint,
        record: withDigest,
        artifacts,
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      assert.equal(wrongSource.available, false);
    } finally {
      key.fill(0);
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "retained loader keeps V1 deterministic reconstruction and rejects unauthenticated metadata",
  ignore: unavailable,
  async fn() {
    const key = new Uint8Array(32).fill(13);
    const keyB64 = encryptedArtifactKey(key);
    const repository = "ubiquity/ai.ubq.fi";
    const baseSha = "7".repeat(40);
    const candidateSha = "8".repeat(40);
    const job = {
      repository,
      issueId: 5228586364,
      nodeId: "I_kwDOQoe6nc8AAAABN6X112",
      number: 112,
      htmlUrl: "https://github.com/ubiquity/ai.ubq.fi/issues/112",
      title: "Retained V1 issue",
      body: "Implement the bounded change.\n\nAcceptance:\n- The change is complete.\n\nFiles:\n- src/http.ts\n",
      bodySha256: "a".repeat(64),
      fingerprint: "9".repeat(64),
      priority: "P3" as const,
      priorityLabel: "Priority: 2 (Medium)",
      queuePriority: null,
      queuePriorityAmbiguous: false,
      timeLabel: "Time: <1 Hour",
      intake: "declared" as const,
      labels: ["Priority: 2 (Medium)", "Time: <1 Hour"],
      files: ["src/http.ts"],
      acceptance: ["The change is complete."],
      materialDigest: null,
      capturedComments: [] as unknown[],
      authorLogin: "0x4007",
      authorAssociation: "MEMBER",
      comments: 0,
      createdAt: "2026-09-01T00:00:00Z",
      updatedAt: "2026-09-01T00:01:00Z",
      relations: {} as never,
    } as unknown as Parameters<typeof loadRetainedIssueFrozenPlan>[0]["job"];
    const interval = {
      schema_version: 1,
      mode: "hourly",
      start: "2026-09-01T00:00:00.000Z",
      end: "2026-09-01T01:00:00.000Z",
      duration_ms: 60 * 60_000,
    };
    // Historical deterministic triage the V1 reproduction must reconstruct.
    const intervalReport = {
      schema_version: 1,
      interval,
      findings: [{
        id: issueJobFindingId(job),
        fingerprint: job.fingerprint,
        severity: "P3" as const,
        title: job.title,
        affected_surface: "src/http.ts",
        allowed_paths: ["src/http.ts"],
        shared_paths: [],
        depends_on: [],
        evidence: [{ source: "github_issue", reference: job.htmlUrl, detail: job.body }],
        proposed_correction:
          "Implement GitHub issue #112 within its declared Files scope and satisfy every acceptance item.",
        validation_requirements: [
          "The change is complete.",
          "Change only declared Files issue paths: src/http.ts",
          "Run repository formatting, lint, build, and affected tests",
        ],
        actionable: true,
      }],
      no_findings_reason: null,
    };
    const v1Selection = {
      schema_version: 1 as const,
      issue_id: job.issueId,
      issue_number: job.number,
      fingerprint: job.fingerprint,
      body_sha256: job.bodySha256,
      comments: 0,
      priority: "P3" as const,
      time_label: "Time: <1 Hour",
      files: ["src/http.ts"],
      updated_at: job.updatedAt,
    };
    const cycle = {
      schema_version: 1,
      run_id: "987654321",
      started_at: "2026-09-01T00:00:00.000Z",
      base_development_sha: baseSha,
    };
    const record = parseSentinelRecoveryRecord({
      schema_version: 1,
      identity: {
        repository,
        source_kind: "github_issue",
        source_id: String(job.issueId),
        source_revision: job.fingerprint,
        candidate_generation: 1,
      },
      run_id: "987654321",
      attempt: 1,
      lease_token: "lease",
      base_sha: baseSha,
      phase: "checkpoint_durable",
      disposition: "active",
      state_version: 4,
      created_at: "2026-09-01T00:00:00.000Z",
      updated_at: "2026-09-01T00:10:00.000Z",
      candidate_branch: "sentinel/candidate-987654321-1",
      candidate_sha: candidateSha,
      changed_files: ["src/http.ts"],
      tree_sha: "4".repeat(40),
      failure_class: null,
      failure_fingerprint: null,
      artifact_ids: [9697049202],
      artifact_digests: [],
      reason: "retained",
      next_action: "resume",
      predecessor: null,
    });
    const checkpoint = { branch: "sentinel/candidate-987654321-1", sha: candidateSha, baseSha };
    const root = await Deno.makeTempDir({ prefix: "sentinel-retained-v1-" });
    try {
      const textEncoder = new TextEncoder();
      const retryReport = {
        schema_version: 1,
        issue_id: job.issueId,
        issue_number: job.number,
        fingerprint: job.fingerprint,
        phase: "failed_implementation",
        implementation_status: "blocked",
        disposition: "retry_pending",
        retry_checkpoint: { branch: checkpoint.branch, sha: checkpoint.sha, base_sha: checkpoint.baseSha },
      };
      const files: SentinelArtifactFile[] = [
        { path: "reports/cycle.json", bytes: textEncoder.encode(JSON.stringify(cycle)) },
        { path: "reports/github-issue-selection.json", bytes: textEncoder.encode(JSON.stringify(v1Selection)) },
        { path: "reports/triage.json", bytes: textEncoder.encode(JSON.stringify(intervalReport)) },
        { path: "reports/recovery-record-v1.json", bytes: textEncoder.encode(JSON.stringify(record)) },
        { path: "reports/github-issue-disposition.json", bytes: textEncoder.encode(JSON.stringify(retryReport)) },
      ];
      const encrypted = await encryptSentinelArtifact(files, key, new Uint8Array(12).fill(3));
      const zip = await createEvidenceZip(root, encrypted);
      const envelope = await decryptSentinelEvidenceEnvelope({ zip, encodedArtifactKey: keyB64, privateRoot: root });
      const withDigest = { ...record, artifact_digests: [envelope.encrypted_digest] };
      const download = (_artifactId: number): Promise<Uint8Array> => Promise.resolve(zip);
      const validArtifact = { workflow_run_id: 987654321, expired: false, expires_at: null, size_in_bytes: 1_024 };
      const base = {
        repository,
        job,
        checkpoint,
        record: withDigest,
        artifacts: [validArtifact],
        download,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      };
      const loaded = await loadRetainedIssueFrozenPlan(base);
      assert.equal(loaded.available, true);
      if (loaded.available) {
        assert.deepEqual(loaded.triage.findings[0]!.allowed_paths, ["src/http.ts"]);
        assert.equal(loaded.selection.schema_version, 1);
      }
      // Metadata must prove the original workflow run; a missing run id rejects.
      const noRun = await loadRetainedIssueFrozenPlan({
        ...base,
        artifacts: [{ expired: false, expires_at: null, size_in_bytes: 1_024 }],
      });
      assert.equal(noRun.available, false);
      if (!noRun.available) assert.equal(noRun.reason.includes("workflow run"), true);
      const expired = await loadRetainedIssueFrozenPlan({
        ...base,
        artifacts: [{ workflow_run_id: 987654321, expired: true, expires_at: null, size_in_bytes: 1_024 }],
      });
      assert.equal(expired.available, false);
      // Corrupt artifact bytes: explicit unavailable, checkpoint preserved.
      const corruptZip = new Uint8Array(64).fill(7);
      const corrupt = await loadRetainedIssueFrozenPlan({
        ...base,
        download: () => Promise.resolve(corruptZip),
      });
      assert.equal(corrupt.available, false);
      // Wrong original attempt: embedded recovery attempt mismatch rejects.
      const wrongAttempt = await loadRetainedIssueFrozenPlan({
        ...base,
        record: { ...withDigest, attempt: 2 },
      });
      assert.equal(wrongAttempt.available, false);
      if (!wrongAttempt.available) {
        assert.equal(wrongAttempt.reason.includes("recovery attempt"), true);
      }
      // Malformed selection schema (valid JSON, invalid schema) fails closed
      // as unavailable with a matching authenticated digest.
      const malformedFiles = files.map((file) =>
        file.path === "reports/github-issue-selection.json"
          ? {
            ...file,
            bytes: textEncoder.encode(JSON.stringify({
              schema_version: 1,
              issue_id: job.issueId,
              issue_number: job.number,
            })),
          }
          : file
      );
      const malformedEncrypted = await encryptSentinelArtifact(malformedFiles, key, new Uint8Array(12).fill(4));
      const malformedZip = await createEvidenceZip(root, malformedEncrypted);
      const malformedEnvelope = await decryptSentinelEvidenceEnvelope({
        zip: malformedZip,
        encodedArtifactKey: keyB64,
        privateRoot: root,
      });
      const malformed = await loadRetainedIssueFrozenPlan({
        ...base,
        download: () => Promise.resolve(malformedZip),
        record: { ...withDigest, artifact_digests: [malformedEnvelope.encrypted_digest] },
      });
      assert.equal(malformed.available, false);
      if (!malformed.available) {
        assert.equal(malformed.reason.includes("malformed"), true);
      }
    } finally {
      key.fill(0);
      await Deno.remove(root, { recursive: true });
    }
  },
});
