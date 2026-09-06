import { decodeSentinelArtifactKey, decryptSentinelArtifact, type SentinelArtifactFile } from "./artifact-crypto.ts";
import { GitHubActionsClient, type GitHubArtifact, type GitHubWorkflowRun } from "./github.ts";
import {
  assertMatrixCellReportDigest,
  assertMatrixCellReportV1,
  type MatrixCellReportV1,
  parseMatrixCellV1,
} from "./matrix.ts";
import {
  matrixCellCanonicalParentIdentity,
  matrixCellRecoverySourceRevision,
  type MatrixCellWorkSelectionV1,
  parseMatrixCellWorkSelection,
} from "./matrix-cell.ts";
import {
  assertSentinelRecoveryTransition,
  parseSentinelRecoveryRecord,
  sentinelRecoveryCandidateBranch as recoveryCandidateBranchForIdentity,
  type SentinelRecoveryIdentityV1,
  type SentinelRecoveryRecordV1,
  type SentinelRecoverySourceKind,
} from "./recovery.ts";
import { isSentinelProtectedImplementationPath } from "./policy.ts";
import {
  parseSentinelRecoveryLedger,
  sentinelRecoveryIdentityKey,
  upsertSentinelRecoveryRecord,
} from "./recovery-ledger.ts";
import {
  readGitHubSentinelRecoveryLedger,
  type SentinelRecoveryLedgerSnapshot,
  writeGitHubSentinelRecoveryLedger,
} from "./recovery-github-store.ts";
import {
  applySentinelRetryPolicyToRecovery,
  type SentinelFailureClass,
  type SentinelRetryAttemptHistory,
  type SentinelRetryDecision,
  stableSentinelFailureFingerprint,
} from "./retry.ts";

const TEXT_DECODER = new TextDecoder("utf-8", { fatal: true });
const TEXT_ENCODER = new TextEncoder();
const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const SAFE_REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const SAFE_BRANCH = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$/u;
const ARTIFACT_NAME = /^sentinel-(?:evidence|matrix-cell)-v1-[A-Za-z0-9._-]+$/u;
const CANDIDATE_MANIFEST =
  /^reports\/(?:(?:failed|manual)-[^/]+-candidate|matrix\/cell-[0-9a-f]{64})\/manifest\.json$/u;
const MATRIX_CELL_DIRECTORY = /^reports\/matrix\/(cell-[0-9a-f]{64})$/u;
const MATRIX_CELL_REPORT_PATH = /^reports\/matrix\/cell-[0-9a-f]{64}\/cell\.json$/u;
const MATRIX_CELL_RECOVERY_RECORD_PATH = /^reports\/matrix\/cell-[0-9a-f]{64}\/recovery-record\.json$/u;
const MATRIX_CELL_WORK_SELECTION_PATH = /^reports\/matrix\/cell-[0-9a-f]{64}\/work-selection\.json$/u;
const matrixCellIdFromPath = (manifestPath: string): string | null =>
  manifestPath.slice(0, manifestPath.lastIndexOf("/")).match(MATRIX_CELL_DIRECTORY)?.[1] ?? null;
const MAX_MANIFEST_BYTES = 512 * 1024;
const MAX_SOURCE_ID_BYTES = 256;
const MAX_SOURCE_REVISION_BYTES = 256;
const MAX_RECOVERY_ARTIFACTS = 128;
export const MAX_ARTIFACT_ZIP_BYTES = 256 * 1024 * 1024;
const ZERO_SHA = "0".repeat(40);
const FIXED_AUTHOR_NAME = "Provider Sentinel Recovery";
const FIXED_AUTHOR_EMAIL = "sentinel-recovery@ubiquity.invalid";
const RECOVERY_SOURCE_KINDS = new Set<SentinelRecoverySourceKind>([
  "github_issue",
  "review_backlog",
  "triage",
  "incident",
]);
const TERMINAL_RECOVERY_DISPOSITIONS = new Set([
  "delivered",
  "rejected",
  "manual_required",
  "resolved",
  "success",
  "successful",
]);
const TERMINAL_CYCLE_STATUSES = new Set([
  "no_change",
  "observed",
  "preview_complete",
  "preview_rolled_back",
  "kept",
  "rolled_back",
]);
const INCOMPLETE_FAILURE_STATUSES = new Set(["failed", "cancelled", "canceled", "timed_out", "timed-out"]);
const TERMINAL_RECORD_REPORT_PATH =
  /^reports\/(?:github-issue-disposition|review-backlog-disposition|recovery(?:-record)?|sentinel-recovery(?:-record)?)\.json$/u;
const TERMINAL_OUTCOME_REPORT_PATH = "reports/github-issue-production-outcome.json";
const MANUAL_CHECKPOINT_REPORT_PATH = "reports/github-issue-manual-checkpoint.json";
const INCOMPLETE_FAILURE_MARKERS = new Set([
  "failure",
  "cancelled",
  "canceled",
  "timed_out",
  "timed-out",
  "timeout",
  "invocation_timeout",
]);

export const SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION = 1 as const;

export type SentinelArtifactRecoveryReason =
  | "recovered"
  | "no_candidate_diff"
  | "artifact_corrupt"
  | "artifact_invalid"
  | "artifact_wrong_base"
  | "artifact_base_unavailable"
  | "candidate_path_forbidden"
  | "candidate_branch_conflict"
  | "recovery_record_invalid"
  | "reconstruction_failed"
  | "terminal_record"
  | "development_head_advanced"
  | "retry_scheduled"
  | "ledger_conflict";

export type SentinelCandidateSnapshotEntry = Readonly<{
  path: string;
  source: "tracked" | "untracked";
  kind: "file" | "symlink" | "deleted";
  mode: number | null;
  size: number;
  sha256: string | null;
  payload: string | null;
  bytes: Uint8Array<ArrayBuffer> | null;
}>;

export type SentinelCandidateSnapshot = Readonly<{
  manifestPath: string;
  baseSha: string;
  capturedAt: string;
  fileCount: number;
  totalBytes: number;
  entries: readonly SentinelCandidateSnapshotEntry[];
  /** Authenticated matrix-cell binding; present only for matrix retry evidence. */
  matrix:
    | Readonly<{
      planDigest: string;
      runId: string;
      runAttempt: number;
      cellContract: ReturnType<typeof parseMatrixCellV1>;
    }>
    | null;
}>;

export type SentinelRecoveryDraftPullRequestRequest = Readonly<{
  method: "POST";
  path: string;
  candidate_sha: string;
  body: Readonly<{
    title: string;
    body: string;
    head: string;
    base: "development";
    draft: true;
    maintainer_can_modify: false;
  }>;
  // GitHub enables auto-merge through a separate mutation. Keep this
  // explicit in the recovery intent while omitting an unsupported REST field.
  auto_merge: false;
  autoMergeEnabled: false;
}>;

export type SentinelRecoveryDraftPullRequest = Readonly<{
  number: number;
  url: string;
  headBranch: string;
  headSha: string;
  baseBranch: "development";
  draft: true;
  autoMergeEnabled: false;
  reused: boolean;
}>;

export type SentinelArtifactRecoveryResult = Readonly<{
  schema_version: typeof SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION;
  disposition: "recovered" | "rejected" | "manual_required" | "retry_pending";
  reason: SentinelArtifactRecoveryReason;
  artifact_digest: string;
  candidate_branch: string | null;
  candidate_sha: string | null;
  tree_sha: string | null;
  changed_files: readonly string[];
  recovery_record: SentinelRecoveryRecordV1 | null;
  draft_pull_request: SentinelRecoveryDraftPullRequestRequest | null;
}>;

type JsonRecord = Record<string, unknown>;

class CandidateSnapshotError extends Error {
  readonly code: SentinelArtifactRecoveryReason;

  constructor(code: SentinelArtifactRecoveryReason) {
    super(code);
    this.name = "CandidateSnapshotError";
    this.code = code;
  }
}

class GitRecoveryError extends Error {
  constructor() {
    // Git output may contain candidate data. Deliberately keep this message
    // categorical so callers can safely put it in a workflow summary.
    super("Sentinel artifact recovery Git operation failed");
    this.name = "GitRecoveryError";
  }
}

const isRecord = (value: unknown): value is JsonRecord =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const hasExactKeys = (value: JsonRecord, expected: readonly string[]): boolean =>
  Object.keys(value).sort().join("\u0000") === [...expected].sort().join("\u0000");

const ensureFullSha = (value: string, label: string): string => {
  if (!FULL_SHA.test(value)) throw new Error(`${label} is not a full Git SHA`);
  return value;
};

const sha256Hex = async (bytes: Uint8Array): Promise<string> =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes))))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");

const isSafeRelativePath = (value: string): boolean => {
  if (
    value.length < 1 || value.length > 1_024 || value.startsWith("/") || value.includes("\\") ||
    value.includes("\0")
  ) return false;
  const parts = value.split("/");
  return parts.every((part) => part.length > 0 && part !== "." && part !== "..") &&
    !value.startsWith(".git/") && value !== ".git";
};

const candidatePathIsAllowed = (path: string): boolean =>
  isSafeRelativePath(path) && !isSentinelProtectedImplementationPath(path);

const positiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const nonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const validIsoTimestamp = (value: unknown): value is string =>
  typeof value === "string" && value.length <= 128 && Number.isFinite(Date.parse(value));

const validSha256 = (value: unknown): value is string => typeof value === "string" && SHA256.test(value);

/**
 * Attestation of the trusted cell-capture gates (exact cell contract, path
 * ownership, and credential scan) persisted beside the candidate manifest.
 * Only the scan report basename and digest are retained, never its content.
 */
const validCaptureAttestation = (value: unknown): boolean =>
  isRecord(value) && value.schema_version === 1 && value.cell_status === "retry_pending" &&
  (value.secret_scan_path === null || typeof value.secret_scan_path === "string") &&
  (value.secret_scan_sha256 === null || validSha256(value.secret_scan_sha256));

const containsAsciiControl = (value: string): boolean =>
  [...value].some((character) => {
    const code = character.charCodeAt(0);
    return code <= 0x1f || code === 0x7f;
  });

const decodeJson = (bytes: Uint8Array, code: SentinelArtifactRecoveryReason): JsonRecord => {
  if (bytes.byteLength > MAX_MANIFEST_BYTES) throw new CandidateSnapshotError(code);
  let parsed: unknown;
  try {
    parsed = JSON.parse(TEXT_DECODER.decode(bytes));
  } catch {
    throw new CandidateSnapshotError(code);
  }
  if (!isRecord(parsed)) throw new CandidateSnapshotError(code);
  return parsed;
};

const comparePaths = (left: string, right: string): number => left < right ? -1 : left > right ? 1 : 0;

const findCandidateManifest = (
  files: readonly SentinelArtifactFile[],
): Readonly<{ file: SentinelArtifactFile; path: string }> => {
  const candidates = files.filter((file) => CANDIDATE_MANIFEST.test(file.path));
  if (candidates.length === 0) throw new CandidateSnapshotError("no_candidate_diff");
  if (candidates.length !== 1) throw new CandidateSnapshotError("artifact_invalid");
  return { file: candidates[0]!, path: candidates[0]!.path };
};

/**
 * Parse and authenticate the candidate snapshot embedded in an already
 * decrypted Sentinel artifact. The returned bytes are owned by the caller and
 * must be zeroed after use.
 */
export const parseSentinelCandidateSnapshot = (
  files: readonly SentinelArtifactFile[],
): SentinelCandidateSnapshot => {
  const selected = findCandidateManifest(files);
  const manifest = decodeJson(selected.file.bytes, "artifact_invalid");
  const legacyKeys = ["base_sha", "captured_at", "file_count", "files", "schema_version", "total_bytes"];
  const matrixKeys = [
    ...legacyKeys,
    "capture_attestation",
    "cell_contract",
    "plan_digest",
    "run_id",
    "run_attempt",
  ];
  const matrixManifest = hasExactKeys(manifest, matrixKeys);
  if (
    !matrixManifest && !hasExactKeys(manifest, legacyKeys) ||
    manifest.schema_version !== 1 || typeof manifest.base_sha !== "string" || !FULL_SHA.test(manifest.base_sha) ||
    !validIsoTimestamp(manifest.captured_at) || !nonNegativeSafeInteger(manifest.file_count) ||
    !nonNegativeSafeInteger(manifest.total_bytes) || !Array.isArray(manifest.files)
  ) throw new CandidateSnapshotError("artifact_invalid");
  if (matrixManifest && !validCaptureAttestation(manifest.capture_attestation)) {
    throw new CandidateSnapshotError("artifact_invalid");
  }
  if (manifest.file_count !== manifest.files.length || manifest.files.length > 1_024) {
    throw new CandidateSnapshotError("artifact_invalid");
  }
  if (!nonNegativeSafeInteger(manifest.total_bytes) || manifest.total_bytes > 64 * 1024 * 1024) {
    throw new CandidateSnapshotError("artifact_invalid");
  }
  let matrix: SentinelCandidateSnapshot["matrix"] = null;
  if (matrixManifest) {
    if (
      typeof manifest.plan_digest !== "string" || !SHA256.test(manifest.plan_digest) ||
      typeof manifest.run_id !== "string" || manifest.run_id.trim().length === 0 ||
      manifest.run_id.length > 64 || !positiveSafeInteger(manifest.run_attempt)
    ) throw new CandidateSnapshotError("artifact_invalid");
    try {
      matrix = {
        planDigest: manifest.plan_digest,
        runId: manifest.run_id,
        runAttempt: manifest.run_attempt,
        cellContract: parseMatrixCellV1(manifest.cell_contract),
      };
    } catch {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    if (matrix.cellContract.base_sha !== manifest.base_sha) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    if (matrix.cellContract.cell_id !== matrixCellIdFromPath(selected.path)) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
  }
  const archiveFiles = new Map(files.map((file) => [file.path, file]));
  const manifestDirectory = selected.path.slice(0, selected.path.lastIndexOf("/"));
  const payloads = new Map<string, SentinelArtifactFile>();
  for (const file of files) {
    if (file.path.startsWith(`${manifestDirectory}/files/`)) payloads.set(file.path, file);
  }
  const entries: SentinelCandidateSnapshotEntry[] = [];
  let totalBytes = 0;
  let previousPath: string | null = null;
  const expectedPayloads = new Set<string>();
  for (const [index, value] of manifest.files.entries()) {
    if (!isRecord(value)) throw new CandidateSnapshotError("artifact_invalid");
    const path = value.path;
    if (typeof path !== "string" || (previousPath !== null && comparePaths(previousPath, path) >= 0)) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    previousPath = path;
    const source = value.source;
    const kind = value.kind;
    if (
      !candidatePathIsAllowed(path) || (source !== "tracked" && source !== "untracked") ||
      (kind !== "file" && kind !== "symlink" && kind !== "deleted")
    ) {
      throw new CandidateSnapshotError("candidate_path_forbidden");
    }
    if (kind === "deleted") {
      if (!hasExactKeys(value, ["kind", "path", "source"]) || source !== "tracked") {
        throw new CandidateSnapshotError("artifact_invalid");
      }
      entries.push({ path, source, kind, mode: null, size: 0, sha256: null, payload: null, bytes: null });
      continue;
    }
    if (!hasExactKeys(value, ["kind", "mode", "path", "payload", "sha256", "size", "source"])) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    if (
      !nonNegativeSafeInteger(value.mode) || value.mode > 0o177777 || !nonNegativeSafeInteger(value.size) ||
      !validSha256(value.sha256) || typeof value.payload !== "string" ||
      !/^files\/[0-9]{4}\.bin$/u.test(value.payload) ||
      value.payload !== `files/${index.toString().padStart(4, "0")}.bin`
    ) throw new CandidateSnapshotError("artifact_invalid");
    const payloadPath = `${manifestDirectory}/${value.payload}`;
    const payload = archiveFiles.get(payloadPath);
    if (!payload || payload.path !== payloadPath || payload.bytes.byteLength !== value.size) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    expectedPayloads.add(payloadPath);
    totalBytes += value.size;
    if (!Number.isSafeInteger(totalBytes) || totalBytes > 64 * 1024 * 1024) {
      throw new CandidateSnapshotError("artifact_invalid");
    }
    entries.push({
      path,
      source,
      kind,
      mode: value.mode,
      size: value.size,
      sha256: value.sha256,
      payload: value.payload,
      bytes: payload.bytes,
    });
  }
  if (totalBytes !== manifest.total_bytes) throw new CandidateSnapshotError("artifact_invalid");
  for (const payloadPath of payloads.keys()) {
    if (!expectedPayloads.has(payloadPath)) throw new CandidateSnapshotError("artifact_invalid");
  }
  const cellId = matrixCellIdFromPath(selected.path);
  if (matrix !== null && cellId === null) throw new CandidateSnapshotError("artifact_invalid");
  return {
    manifestPath: selected.path,
    baseSha: manifest.base_sha as string,
    capturedAt: manifest.captured_at as string,
    fileCount: manifest.file_count as number,
    totalBytes: manifest.total_bytes as number,
    entries,
    matrix,
  };
};

const pathOverlaps = (left: string, right: string): boolean =>
  left === right || left.startsWith(`${right}/`) || right.startsWith(`${left}/`);

const pathInScope = (path: string, scopes: readonly string[]): boolean =>
  scopes.some((scope) => pathOverlaps(path, scope));

/**
 * Re-scan the reconstructed matrix snapshot against the exact immutable cell
 * contract inside the authenticated manifest before any branch push or draft
 * PR. Any path outside ownership fails closed to manual review and no byte is
 * ever published.
 */
const assertMatrixSnapshotOwnership = (
  cell: ReturnType<typeof parseMatrixCellV1>,
  snapshot: SentinelCandidateSnapshot,
): void => {
  for (const entry of snapshot.entries) {
    if (
      !candidatePathIsAllowed(entry.path) || !pathInScope(entry.path, cell.allowed_paths) ||
      pathInScope(entry.path, cell.prohibited_paths) || pathInScope(entry.path, cell.shared_paths)
    ) {
      throw new CandidateSnapshotError("candidate_path_forbidden");
    }
    // Matrix retry evidence never contains symlinks; a reconstructed symlink
    // would escape the trusted single-level snapshot model.
    if (entry.kind === "symlink") throw new CandidateSnapshotError("artifact_invalid");
  }
};

const MATRIX_CELL_LEASE = /^matrix-cell-[0-9]+-[1-9][0-9]*-cell-[0-9a-f]{64}$/u;

const assertMatrixRecordSnapshotBinding = (
  record: SentinelRecoveryRecordV1,
  matrix: NonNullable<SentinelCandidateSnapshot["matrix"]>,
): void => {
  if (
    !MATRIX_CELL_LEASE.test(record.lease_token) ||
    record.lease_token !== `matrix-cell-${matrix.runId}-${matrix.runAttempt}-${matrix.cellContract.cell_id}` ||
    record.run_id !== matrix.runId
  ) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
};

const stableHash = (value: string): string => {
  let hash = 2_166_136_261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16_777_619) >>> 0;
  }
  return hash.toString(16).padStart(8, "0");
};

const slug = (value: string, fallback: string): string => {
  const result = value.replace(/[^A-Za-z0-9._-]+/gu, "-").replace(/^-+|-+$/gu, "");
  return result || fallback;
};

const validateRecoveryIdentity = (record: SentinelRecoveryRecordV1): void => {
  if (
    !SAFE_REPOSITORY.test(record.identity.repository) || !RECOVERY_SOURCE_KINDS.has(record.identity.source_kind) ||
    record.identity.source_id.trim().length === 0 || record.identity.source_revision.trim().length === 0 ||
    !positiveSafeInteger(record.identity.candidate_generation)
  ) throw new CandidateSnapshotError("recovery_record_invalid");
  if (
    new TextEncoder().encode(record.identity.source_id).byteLength > MAX_SOURCE_ID_BYTES ||
    new TextEncoder().encode(record.identity.source_revision).byteLength > MAX_SOURCE_REVISION_BYTES
  ) throw new CandidateSnapshotError("recovery_record_invalid");
  if ([record.identity.source_id, record.identity.source_revision].some(containsAsciiControl)) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
};

/** Return the deterministic quarantine branch shared with issue reconciliation. */
export const sentinelRecoveryCandidateBranch = (record: SentinelRecoveryRecordV1): string => {
  validateRecoveryIdentity(record);
  const branch = recoveryCandidateBranchForIdentity(record.identity);
  if (!SAFE_BRANCH.test(branch)) throw new CandidateSnapshotError("recovery_record_invalid");
  return branch;
};

const recoveryIdentityMarker = (record: SentinelRecoveryRecordV1): string => {
  const identity = [
    record.identity.repository,
    record.identity.source_kind,
    record.identity.source_id,
    record.identity.source_revision,
    record.identity.candidate_generation,
  ].join("\u0000");
  return stableHash(identity);
};

const safeSourceKind = (sourceKind: SentinelRecoverySourceKind): string => slug(sourceKind, "source");

/** Build a GitHub REST create-PR request; it never enables auto-merge. */
export const buildSentinelRecoveryDraftPullRequest = (
  input: Readonly<{
    repository: string;
    record: SentinelRecoveryRecordV1;
    candidateBranch: string;
    candidateSha: string;
    artifactId?: number | null;
  }>,
): SentinelRecoveryDraftPullRequestRequest => {
  validateRecoveryIdentity(input.record);
  if (!SAFE_REPOSITORY.test(input.repository) || input.repository !== input.record.identity.repository) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
  if (!SAFE_BRANCH.test(input.candidateBranch) || input.candidateBranch.includes("..")) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
  ensureFullSha(input.candidateSha, "Candidate SHA");
  if (input.artifactId !== undefined && input.artifactId !== null && !positiveSafeInteger(input.artifactId)) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
  const marker = `<!-- provider-sentinel:artifact-recovery:v1 identity=${recoveryIdentityMarker(input.record)} -->`;
  const body = [
    marker,
    "",
    `## Provider Sentinel recovered candidate (${safeSourceKind(input.record.identity.source_kind)})`,
    "",
    "This is a quarantined draft recovery request. It is not an approval, merge, deployment, or delivery signal.",
    "",
    `- Source: \`${safeSourceKind(input.record.identity.source_kind)}:${
      slug(input.record.identity.source_id, "item")
    }\``,
    `- Source revision: \`${slug(input.record.identity.source_revision, "revision")}\``,
    `- Candidate generation: \`${input.record.identity.candidate_generation}\``,
    `- Base SHA: \`${input.record.base_sha}\``,
    `- Candidate SHA: \`${input.candidateSha}\``,
    `- Candidate branch: \`${input.candidateBranch}\``,
    `- Evidence artifact ID: \`${input.artifactId ?? "unavailable"}\``,
    "",
    "Auto-merge is disabled. A human must validate the candidate and use the protected delivery workflow.",
    "",
  ].join("\n");
  return {
    method: "POST",
    path: `/repos/${input.repository}/pulls`,
    candidate_sha: input.candidateSha,
    body: {
      title: `chore(sentinel): recover ${safeSourceKind(input.record.identity.source_kind)} candidate`,
      body,
      head: input.candidateBranch,
      base: "development",
      draft: true,
      maintainer_can_modify: false,
    },
    auto_merge: false,
    autoMergeEnabled: false,
  };
};

const runGit = async (
  cwd: string,
  args: readonly string[],
  env?: Readonly<Record<string, string>>,
): Promise<Uint8Array<ArrayBuffer>> => {
  const command = new Deno.Command("git", {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
    env: env === undefined ? undefined : { ...env },
  });
  const output = await command.output();
  if (!output.success) throw new GitRecoveryError();
  return output.stdout;
};

const gitText = async (
  cwd: string,
  args: readonly string[],
  env?: Readonly<Record<string, string>>,
): Promise<string> => TEXT_DECODER.decode(await runGit(cwd, args, env)).trim();

const optionalGitText = async (
  cwd: string,
  args: readonly string[],
  env?: Readonly<Record<string, string>>,
): Promise<string | null> => {
  const command = new Deno.Command("git", {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
    env: env === undefined ? undefined : { ...env },
  });
  const output = await command.output();
  if (output.success) return TEXT_DECODER.decode(output.stdout).trim();
  return null;
};

const assertDigest = async (entry: SentinelCandidateSnapshotEntry): Promise<void> => {
  if (!entry.bytes || !entry.sha256 || entry.kind === "deleted") return;
  if (await sha256Hex(entry.bytes) !== entry.sha256) throw new CandidateSnapshotError("artifact_invalid");
};

const removeExistingCandidatePath = async (path: string): Promise<void> => {
  let information: Deno.FileInfo;
  try {
    information = await Deno.lstat(path);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return;
    throw error;
  }
  if (information.isDirectory && !information.isSymlink) throw new CandidateSnapshotError("artifact_invalid");
  await Deno.remove(path);
};

const ensureCandidateParent = async (checkout: string, path: string): Promise<void> => {
  const parts = path.split("/");
  let current = checkout;
  for (const part of parts.slice(0, -1)) {
    current = `${current}/${part}`;
    try {
      const information = await Deno.lstat(current);
      if (information.isSymlink || !information.isDirectory) throw new CandidateSnapshotError("artifact_invalid");
    } catch (error) {
      if (!(error instanceof Deno.errors.NotFound)) throw error;
      await Deno.mkdir(current, { mode: 0o755 });
    }
  }
};

const applyCandidateSnapshot = async (
  checkout: string,
  snapshot: SentinelCandidateSnapshot,
): Promise<void> => {
  for (const entry of snapshot.entries) {
    await ensureCandidateParent(checkout, entry.path);
    const target = `${checkout}/${entry.path}`;
    if (entry.kind === "deleted") {
      await removeExistingCandidatePath(target);
      continue;
    }
    if (!entry.bytes || entry.mode === null) throw new CandidateSnapshotError("artifact_invalid");
    await removeExistingCandidatePath(target);
    if (entry.kind === "symlink") {
      let linkTarget: string;
      try {
        linkTarget = TEXT_DECODER.decode(entry.bytes);
      } catch {
        throw new CandidateSnapshotError("artifact_invalid");
      }
      if (linkTarget.length === 0 || linkTarget.startsWith("/") || linkTarget.includes("\0")) {
        throw new CandidateSnapshotError("artifact_invalid");
      }
      await Deno.symlink(linkTarget, target);
    } else {
      await Deno.writeFile(target, entry.bytes, { mode: 0o600 });
      await Deno.chmod(target, entry.mode & 0o7777);
    }
  }
};

const changedPathsAtIndex = async (checkout: string): Promise<string[]> => {
  const output = await runGit(checkout, ["diff", "--cached", "--name-only", "-z"]);
  return TEXT_DECODER.decode(output).split("\0").filter(Boolean).sort(comparePaths);
};

const createDeterministicCommit = async (
  root: string,
  baseSha: string,
  snapshot: SentinelCandidateSnapshot,
  record: SentinelRecoveryRecordV1,
): Promise<Readonly<{ candidateSha: string; treeSha: string }>> => {
  const tempRoot = await Deno.makeTempDir({ prefix: "sentinel-artifact-recovery-" });
  const checkout = `${tempRoot}/candidate`;
  let worktreeAdded = false;
  try {
    await Deno.mkdir(checkout, { recursive: true, mode: 0o700 });
    await runGit(root, ["worktree", "add", "--detach", checkout, baseSha]);
    worktreeAdded = true;
    await applyCandidateSnapshot(checkout, snapshot);
    await runGit(checkout, ["add", "--all", "--", "."]);
    const changed = await changedPathsAtIndex(checkout);
    const expected = snapshot.entries.map((entry) => entry.path).sort(comparePaths);
    if (JSON.stringify(changed) !== JSON.stringify(expected)) throw new CandidateSnapshotError("artifact_invalid");
    const treeSha = ensureFullSha(await gitText(checkout, ["write-tree"]), "Candidate tree SHA");
    const identity = recoveryIdentityMarker(record);
    const message = `chore(sentinel): recover encrypted candidate ${identity}`;
    const timestamp = new Date(Date.parse(record.created_at)).toISOString();
    const commitOutput = await runGit(checkout, ["commit-tree", treeSha, "-p", baseSha, "-m", message], {
      GIT_AUTHOR_NAME: FIXED_AUTHOR_NAME,
      GIT_AUTHOR_EMAIL: FIXED_AUTHOR_EMAIL,
      GIT_AUTHOR_DATE: timestamp,
      GIT_COMMITTER_NAME: FIXED_AUTHOR_NAME,
      GIT_COMMITTER_EMAIL: FIXED_AUTHOR_EMAIL,
      GIT_COMMITTER_DATE: timestamp,
    });
    const candidateSha = ensureFullSha(TEXT_DECODER.decode(commitOutput).trim(), "Candidate commit SHA");
    const parents = await gitText(checkout, ["rev-list", "--parents", "-n", "1", candidateSha]);
    if (parents !== `${candidateSha} ${baseSha}`) throw new CandidateSnapshotError("artifact_invalid");
    return { candidateSha, treeSha };
  } finally {
    if (worktreeAdded) {
      await runGit(root, ["worktree", "remove", "--force", checkout]).catch(() => undefined);
    }
    await Deno.remove(tempRoot, { recursive: true }).catch(() => undefined);
  }
};

const localRefSha = async (root: string, ref: string): Promise<string | null> =>
  await optionalGitText(root, ["rev-parse", "--verify", `${ref}^{commit}`]);

const publishLocalRecoveryRef = async (
  root: string,
  branch: string,
  candidateSha: string,
): Promise<void> => {
  const ref = `refs/heads/${branch}`;
  const existing = await localRefSha(root, ref);
  if (existing !== null && existing !== candidateSha) throw new CandidateSnapshotError("candidate_branch_conflict");
  if (existing === candidateSha) return;
  const remote = await localRefSha(root, `refs/remotes/origin/${branch}`);
  if (remote !== null && remote !== candidateSha) throw new CandidateSnapshotError("candidate_branch_conflict");
  const expected = existing ?? ZERO_SHA;
  await runGit(root, ["update-ref", ref, candidateSha, expected]);
  if (await localRefSha(root, ref) !== candidateSha) throw new GitRecoveryError();
};

const withCandidateBytesZeroed = async <T>(
  files: readonly SentinelArtifactFile[],
  operation: () => Promise<T>,
): Promise<T> => {
  try {
    return await operation();
  } finally {
    for (const file of files) file.bytes.fill(0);
  }
};

const artifactDigest = async (encryptedBytes: Uint8Array): Promise<string> =>
  `sha256:${await sha256Hex(encryptedBytes)}`;

const safeRecordWithArtifact = (
  record: SentinelRecoveryRecordV1,
  digest: string,
  artifactId?: number,
): SentinelRecoveryRecordV1 => {
  const artifactDigests = [...new Set([...record.artifact_digests, digest])].sort();
  const artifactIds = artifactId === undefined
    ? record.artifact_ids
    : [...new Set([...record.artifact_ids, artifactId])].sort((left, right) => left - right);
  return { ...record, artifact_digests: artifactDigests, artifact_ids: artifactIds };
};

const transitionRecord = (
  record: SentinelRecoveryRecordV1,
  next: Readonly<Partial<SentinelRecoveryRecordV1> & { phase: SentinelRecoveryRecordV1["phase"] }>,
): SentinelRecoveryRecordV1 => {
  const candidate = parseSentinelRecoveryRecord({
    ...record,
    ...next,
    state_version: record.state_version + 1,
    updated_at: new Date(Math.max(Date.now(), Date.parse(record.updated_at))).toISOString(),
  });
  assertSentinelRecoveryTransition(record, candidate);
  return candidate;
};

const manualResult = async (
  record: SentinelRecoveryRecordV1 | null,
  digest: string,
  reason: SentinelArtifactRecoveryReason,
): Promise<SentinelArtifactRecoveryResult> => {
  if (!record) {
    return {
      schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
      disposition: "manual_required",
      reason,
      artifact_digest: digest,
      candidate_branch: null,
      candidate_sha: null,
      tree_sha: null,
      changed_files: [],
      recovery_record: null,
      draft_pull_request: null,
    };
  }
  let recoveryRecord: SentinelRecoveryRecordV1 | null = null;
  try {
    recoveryRecord = transitionRecord(record, {
      phase: "manual_required",
      disposition: "manual_required",
      candidate_branch: null,
      candidate_sha: null,
      changed_files: [],
      tree_sha: null,
      failure_class: reason,
      failure_fingerprint: await sha256Hex(TEXT_ENCODER.encode(reason)),
      reason,
      next_action: "manual inspection of encrypted evidence and the exact base is required",
    });
  } catch {
    // A stale or terminal predecessor must never be rewritten. Returning the
    // safe result still lets the controller retain the evidence for an owner.
    recoveryRecord = null;
  }
  return {
    schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
    disposition: "manual_required",
    reason,
    artifact_digest: digest,
    candidate_branch: null,
    candidate_sha: null,
    tree_sha: null,
    changed_files: [],
    recovery_record: recoveryRecord,
    draft_pull_request: null,
  };
};

const rejectedResult = async (
  record: SentinelRecoveryRecordV1,
  digest: string,
): Promise<SentinelArtifactRecoveryResult> => {
  let recoveryRecord: SentinelRecoveryRecordV1 | null = null;
  try {
    recoveryRecord = transitionRecord(record, {
      phase: "rejected",
      disposition: "rejected",
      candidate_branch: null,
      candidate_sha: null,
      changed_files: [],
      tree_sha: null,
      failure_class: "no_candidate_diff",
      failure_fingerprint: await sha256Hex(TEXT_ENCODER.encode("no_candidate_diff")),
      reason: "no_candidate_diff",
      next_action: null,
    });
  } catch {
    recoveryRecord = null;
  }
  return {
    schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
    disposition: "rejected",
    reason: "no_candidate_diff",
    artifact_digest: digest,
    candidate_branch: null,
    candidate_sha: null,
    tree_sha: null,
    changed_files: [],
    recovery_record: recoveryRecord,
    draft_pull_request: null,
  };
};

/**
 * Recover one encrypted evidence envelope into an immutable quarantine branch.
 * Every parse, digest, base, and Git failure becomes a categorical
 * `manual_required` result; candidate bytes are never included in a result.
 */
export const recoverSentinelArtifactCandidate = async (
  input: Readonly<{
    checkout: string;
    encryptedBytes: Uint8Array<ArrayBuffer>;
    keyBytes: Uint8Array<ArrayBuffer>;
    record: unknown;
    candidateBranch?: string;
    expectedBaseSha?: string;
    artifactId?: number;
  }>,
): Promise<SentinelArtifactRecoveryResult> => {
  const digest = await artifactDigest(input.encryptedBytes).catch(() => "sha256:unknown");
  let record: SentinelRecoveryRecordV1;
  try {
    record = parseSentinelRecoveryRecord(input.record);
    validateRecoveryIdentity(record);
    if (record.phase !== "recovery_pending") throw new CandidateSnapshotError("recovery_record_invalid");
    if (input.expectedBaseSha !== undefined) ensureFullSha(input.expectedBaseSha, "Expected development base SHA");
    record = safeRecordWithArtifact(record, digest, input.artifactId);
    if (input.expectedBaseSha !== undefined && input.expectedBaseSha !== record.base_sha) {
      return await manualResult(record, digest, "artifact_wrong_base");
    }
  } catch {
    return await manualResult(null, digest, "recovery_record_invalid");
  }

  let decrypted: SentinelArtifactFile[] = [];
  try {
    decrypted = await decryptSentinelArtifact(input.encryptedBytes, input.keyBytes);
  } catch {
    return await manualResult(record, digest, "artifact_corrupt");
  }
  return await withCandidateBytesZeroed(decrypted, async () => {
    let snapshot: SentinelCandidateSnapshot;
    try {
      snapshot = parseSentinelCandidateSnapshot(decrypted);
      if (snapshot.fileCount === 0) return await rejectedResult(record, digest);
      for (const entry of snapshot.entries) await assertDigest(entry);
      if (snapshot.matrix !== null) {
        assertMatrixSnapshotOwnership(snapshot.matrix.cellContract, snapshot);
        assertMatrixRecordSnapshotBinding(record, snapshot.matrix);
      }
      if (snapshot.baseSha !== record.base_sha) return await manualResult(record, digest, "artifact_wrong_base");
      const base = await localRefSha(input.checkout, `${record.base_sha}^{commit}`);
      if (base !== record.base_sha) return await manualResult(record, digest, "artifact_base_unavailable");
      const branch = input.candidateBranch ?? sentinelRecoveryCandidateBranch(record);
      if (!SAFE_BRANCH.test(branch) || branch.includes("..")) {
        return await manualResult(record, digest, "recovery_record_invalid");
      }
      const { candidateSha, treeSha } = await createDeterministicCommit(
        input.checkout,
        record.base_sha,
        snapshot,
        record,
      );
      await publishLocalRecoveryRef(input.checkout, branch, candidateSha);
      const nextRecord = transitionRecord(record, {
        phase: "checkpoint_durable",
        disposition: "active",
        candidate_branch: branch,
        candidate_sha: candidateSha,
        changed_files: snapshot.entries.map((entry) => entry.path),
        tree_sha: treeSha,
        // The original classified cell failure stays durable on the
        // checkpoint: it is the only evidence of why the cell ended
        // retry_pending and feeds the bounded retry circuit.
        failure_class: record.failure_class,
        failure_fingerprint: record.failure_fingerprint,
        reason: "encrypted candidate recovered into a quarantined checkpoint",
        next_action: "validate the checkpoint and request human review",
      });
      const draftPullRequest = buildSentinelRecoveryDraftPullRequest({
        repository: record.identity.repository,
        record: nextRecord,
        candidateBranch: branch,
        candidateSha,
        artifactId: input.artifactId,
      });
      return {
        schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
        disposition: "recovered",
        reason: "recovered",
        artifact_digest: digest,
        candidate_branch: branch,
        candidate_sha: candidateSha,
        tree_sha: treeSha,
        changed_files: snapshot.entries.map((entry) => entry.path),
        recovery_record: nextRecord,
        draft_pull_request: draftPullRequest,
      };
    } catch (error) {
      const reason = error instanceof CandidateSnapshotError ? error.code : "reconstruction_failed";
      return await manualResult(record, digest, reason);
    }
  });
};

export const recoverEncryptedSentinelCandidate = recoverSentinelArtifactCandidate;

type GitHubRequest = (
  input: string | URL | Request,
  init?: RequestInit,
) => Promise<Response>;

const githubResponseJson = async (response: Response): Promise<unknown> => {
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.byteLength > 2 * 1024 * 1024) throw new Error("GitHub response exceeded the recovery limit");
  if (bytes.byteLength === 0) return null;
  try {
    return JSON.parse(TEXT_DECODER.decode(bytes));
  } catch {
    throw new Error("GitHub response was not JSON");
  }
};

const pullRequestIdentity = (value: unknown):
  | Readonly<{
    number: number;
    url: string;
    state: "open" | "closed";
    mergedAt: string | null;
    headBranch: string;
    headSha: string;
    baseBranch: string;
    draft: boolean;
    autoMerge: unknown;
    body: string;
  }>
  | null => {
  if (
    !isRecord(value) || !positiveSafeInteger(value.number) || typeof value.html_url !== "string" ||
    (value.state !== "open" && value.state !== "closed") ||
    (value.merged_at !== null && typeof value.merged_at !== "string") ||
    !(value.body === null || typeof value.body === "string")
  ) return null;
  const head = isRecord(value.head) ? value.head : null;
  const base = isRecord(value.base) ? value.base : null;
  if (
    !head || !base || typeof head.ref !== "string" || !FULL_SHA.test(String(head.sha)) ||
    typeof base.ref !== "string" || typeof value.draft !== "boolean"
  ) return null;
  return {
    number: value.number,
    url: value.html_url,
    state: value.state,
    mergedAt: value.merged_at,
    headBranch: head.ref,
    headSha: String(head.sha),
    baseBranch: base.ref,
    draft: value.draft,
    autoMerge: value.auto_merge,
    body: value.body ?? "",
  };
};

const githubFetch = async (
  token: string,
  repository: string,
  path: string,
  init: RequestInit,
  fetcher: GitHubRequest,
): Promise<unknown> => {
  if (!SAFE_REPOSITORY.test(repository)) throw new Error("Recovery repository identity is invalid");
  const response = await fetcher(`https://api.github.com${path}`, {
    ...init,
    headers: {
      Accept: "application/vnd.github+json",
      Authorization: `Bearer ${token}`,
      "X-GitHub-Api-Version": "2026-03-10",
      ...(init.body === undefined ? {} : { "Content-Type": "application/json" }),
      ...init.headers,
    },
    signal: AbortSignal.timeout(20_000),
  });
  const value = await githubResponseJson(response);
  if (!response.ok) throw new Error(`GitHub recovery request failed with HTTP ${response.status}`);
  return value;
};

/** Create or reuse exactly one draft recovery PR, with auto-merge off. */
export const createOrReuseSentinelRecoveryDraftPullRequest = async (
  input: Readonly<{
    token: string;
    request: SentinelRecoveryDraftPullRequestRequest;
    fetcher?: GitHubRequest;
  }>,
): Promise<SentinelRecoveryDraftPullRequest> => {
  const fetcher = input.fetcher ?? fetch;
  const match = input.request.path.match(/^\/repos\/([^/]+\/[^/]+)\/pulls$/u);
  if (!match || input.token.trim() === "") throw new Error("Recovery pull-request request is invalid");
  const repository = match[1]!;
  const request = input.request;
  if (
    request.method !== "POST" || request.auto_merge !== false || request.autoMergeEnabled !== false ||
    !FULL_SHA.test(request.candidate_sha) || request.body.base !== "development" || request.body.draft !== true ||
    request.body.maintainer_can_modify !== false || !SAFE_BRANCH.test(request.body.head) ||
    request.body.head.includes("..") || request.body.head.startsWith("refs/")
  ) throw new Error("Recovery pull-request request is not draft-only");
  const marker = request.body.body.split("\n", 1)[0]!;
  const existingValue = await githubFetch(
    input.token,
    repository,
    `/repos/${repository}/pulls?state=open&base=development&per_page=100`,
    { method: "GET" },
    fetcher,
  );
  if (!Array.isArray(existingValue)) throw new Error("GitHub recovery pull-request listing is invalid");
  const pulls = existingValue.map(pullRequestIdentity);
  if (pulls.some((pull) => pull === null)) throw new Error("GitHub recovery pull-request identity is invalid");
  const matches = pulls.filter((pull) =>
    pull !== null && pull.body.includes(marker) && pull.headBranch === request.body.head &&
    pull.baseBranch === "development"
  ) as NonNullable<ReturnType<typeof pullRequestIdentity>>[];
  if (matches.length > 1) throw new Error("More than one Sentinel recovery draft pull request exists");
  let pull: NonNullable<ReturnType<typeof pullRequestIdentity>> | null = matches[0] ?? null;
  let reused = false;
  if (pull) {
    if (
      pull.state !== "open" || pull.mergedAt !== null || pull.headSha !== request.candidate_sha ||
      pull.autoMerge !== null && pull.autoMerge !== undefined
    ) {
      throw new Error("Existing Sentinel recovery pull request is not a safe draft");
    }
    reused = true;
    // An exact replay of the same artifact reuses the identical draft PR without
    // any mutation: only a changed body is patched, and only after a fresh
    // identity re-read of the same pull request.
    if (pull.body !== request.body.body) {
      const fetchResult = await githubFetch(input.token, repository, `/repos/${repository}/pulls/${pull.number}`, {
        method: "PATCH",
        body: JSON.stringify({ body: request.body.body, draft: true }),
      }, fetcher);
      pull = pullRequestIdentity(fetchResult);
    }
  } else {
    let created: unknown = null;
    try {
      created = await githubFetch(input.token, repository, request.path, {
        method: "POST",
        body: JSON.stringify(request.body),
      }, fetcher);
    } catch (error) {
      // A concurrent recovery may have created the identical draft PR first
      // (GitHub refuses a second PR for the same head). Re-list and reuse.
      const retried = await githubFetch(
        input.token,
        repository,
        `/repos/${repository}/pulls?state=open&base=development&per_page=100`,
        { method: "GET" },
        fetcher,
      );
      if (!Array.isArray(retried)) throw error;
      const retriedPulls = retried.map(pullRequestIdentity);
      if (retriedPulls.some((pull) => pull === null)) throw error;
      const retriedMatches = retriedPulls.filter((pull) =>
        pull !== null && pull.body.includes(marker) && pull.headBranch === request.body.head &&
        pull.baseBranch === "development"
      ) as NonNullable<ReturnType<typeof pullRequestIdentity>>[];
      if (retriedMatches.length !== 1) throw error;
      pull = retriedMatches[0];
      reused = true;
    }
    pull = pull ?? pullRequestIdentity(created);
  }
  if (
    !pull || pull.state !== "open" || pull.mergedAt !== null || !pull.draft ||
    pull.autoMerge !== null && pull.autoMerge !== undefined ||
    pull.headBranch !== request.body.head || pull.headSha !== request.candidate_sha ||
    pull.baseBranch !== "development" ||
    !pull.body.includes(marker)
  ) {
    throw new Error("Sentinel recovery pull request failed its draft-only identity check");
  }
  return {
    number: pull.number,
    url: pull.url,
    headBranch: pull.headBranch,
    headSha: pull.headSha,
    baseBranch: "development",
    draft: true,
    autoMergeEnabled: false,
    reused,
  };
};

const parseArtifactJson = (files: readonly SentinelArtifactFile[], path: string): JsonRecord | null => {
  const file = files.find((candidate) => candidate.path === path);
  if (!file) return null;
  try {
    return decodeJson(file.bytes, "artifact_invalid");
  } catch {
    return null;
  }
};

const terminalRecordState = (value: JsonRecord): boolean =>
  [value.phase, value.disposition, value.status].some((state) =>
    typeof state === "string" && TERMINAL_RECOVERY_DISPOSITIONS.has(state)
  );

const explicitIncompleteFailureEvidence = (value: JsonRecord | null): boolean => {
  if (!value) return false;
  if (value.codex_timed_out === true) return true;
  return [
    value.codex_failure,
    value.failure,
    value.reason,
    value.status,
    value.conclusion,
    value.workflow_conclusion,
    value.outcome,
  ].some((marker) => typeof marker === "string" && INCOMPLETE_FAILURE_MARKERS.has(marker));
};

const terminalArtifactRecord = (files: readonly SentinelArtifactFile[]): boolean => {
  const reports = files.filter((file) => TERMINAL_RECORD_REPORT_PATH.test(file.path));
  for (const report of reports) {
    const value = parseArtifactJson(files, report.path);
    // A malformed terminal-state report is not permission to reconstruct a
    // candidate. Keep the artifact owner-visible without mutating Git.
    if (value === null || terminalRecordState(value)) return true;
  }
  const outcomeFile = files.find((file) => file.path === TERMINAL_OUTCOME_REPORT_PATH);
  // Any production-outcome report means the candidate reached a terminal
  // runtime decision. Unknown or malformed values are not safe to replay.
  if (outcomeFile) return true;
  // Native review exhaustion already has a human checkpoint. It is terminal
  // for this recovery job even when a failed preservation report is present.
  const manualCheckpoint = files.find((file) => file.path === MANUAL_CHECKPOINT_REPORT_PATH);
  return manualCheckpoint !== undefined;
};

/**
 * Return true only for an encrypted artifact that proves it contains an
 * incomplete failed, cancelled, or timed-out candidate. Terminal cycle and
 * delivery records are deliberately fail-closed before any Git mutation.
 */
export const isSentinelArtifactRecoveryEligible = (
  files: readonly SentinelArtifactFile[],
  workflowRun?: Readonly<Pick<GitHubWorkflowRun, "status" | "conclusion">>,
): boolean => {
  const candidateManifests = files.filter((file) => CANDIDATE_MANIFEST.test(file.path));
  if (candidateManifests.length !== 1) return false;
  const cycle = parseArtifactJson(files, "reports/cycle.json");
  if (!cycle || cycle.schema_version !== 1 || terminalRecordState(cycle) || terminalArtifactRecord(files)) return false;
  const status = typeof cycle.status === "string" ? cycle.status : null;
  if (!status || TERMINAL_CYCLE_STATUSES.has(status)) return false;
  if (INCOMPLETE_FAILURE_STATUSES.has(status)) return true;
  if (status !== "running") return false;
  return explicitIncompleteFailureEvidence(parseArtifactJson(files, "reports/failure.json")) ||
    workflowRun?.status === "completed" &&
      typeof workflowRun.conclusion === "string" &&
      INCOMPLETE_FAILURE_MARKERS.has(workflowRun.conclusion);
};

export const legacyArtifactNeedsManualDisposition = (
  files: readonly SentinelArtifactFile[],
  workflowRun?: Readonly<Pick<GitHubWorkflowRun, "status" | "conclusion">>,
): boolean => {
  const cycle = parseArtifactJson(files, "reports/cycle.json");
  const status = typeof cycle?.status === "string" ? cycle.status : null;
  const ciphertextProvesFailure = status !== null && INCOMPLETE_FAILURE_STATUSES.has(status) ||
    explicitIncompleteFailureEvidence(parseArtifactJson(files, "reports/failure.json"));
  const owningRunProvesFailure = workflowRun?.status === "completed" &&
    typeof workflowRun.conclusion === "string" &&
    INCOMPLETE_FAILURE_MARKERS.has(workflowRun.conclusion);
  return !terminalArtifactRecord(files) &&
    (status === null || !TERMINAL_CYCLE_STATUSES.has(status)) &&
    (ciphertextProvesFailure || owningRunProvesFailure);
};

export const legacyArtifactTerminalDisposition = (
  files: readonly SentinelArtifactFile[],
): "manual_required" | "rejected" | "delivered" | null => {
  if (files.some((file) => file.path === MANUAL_CHECKPOINT_REPORT_PATH)) return "manual_required";
  for (const file of files.filter((candidate) => TERMINAL_RECORD_REPORT_PATH.test(candidate.path))) {
    const value = parseArtifactJson(files, file.path);
    const disposition = value?.disposition;
    if (disposition === "manual_required") return "manual_required";
    if (disposition === "rejected") return "rejected";
    if (disposition === "resolved" || disposition === "delivered") return "delivered";
  }
  const outcome = parseArtifactJson(files, TERMINAL_OUTCOME_REPORT_PATH)?.outcome;
  if (outcome === "kept") return "delivered";
  if (outcome === "rolled_back") return "rejected";
  const cycleStatus = parseArtifactJson(files, "reports/cycle.json")?.status;
  if (cycleStatus === "kept") return "delivered";
  if (typeof cycleStatus === "string" && TERMINAL_CYCLE_STATUSES.has(cycleStatus)) return "rejected";
  return null;
};

export const legacyArtifactHasTerminalReport = (files: readonly SentinelArtifactFile[]): boolean =>
  files.some((file) => TERMINAL_RECORD_REPORT_PATH.test(file.path)) ||
  files.some((file) => file.path === TERMINAL_OUTCOME_REPORT_PATH || file.path === MANUAL_CHECKPOINT_REPORT_PATH);

const textField = (value: unknown, maximumBytes = 512): string | null =>
  typeof value === "string" && value.length > 0 && new TextEncoder().encode(value).byteLength <= maximumBytes &&
    !containsAsciiControl(value)
    ? value
    : null;

const recordFromArtifact = (
  files: readonly SentinelArtifactFile[],
  artifact: GitHubArtifact,
  encryptedDigest: string,
  workflowRun?: Readonly<Pick<GitHubWorkflowRun, "status" | "conclusion">>,
): SentinelRecoveryRecordV1 | null => {
  if (!isSentinelArtifactRecoveryEligible(files, workflowRun)) return null;
  const cycle = parseArtifactJson(files, "reports/cycle.json");
  if (!cycle || cycle.schema_version !== 1) return null;
  const runId = textField(cycle.run_id, 64);
  const baseSha = typeof cycle.base_development_sha === "string" && FULL_SHA.test(cycle.base_development_sha)
    ? cycle.base_development_sha
    : null;
  const startedAt = validIsoTimestamp(cycle.started_at) ? cycle.started_at : null;
  if (!runId || !baseSha || !startedAt) return null;
  const issue = parseArtifactJson(files, "reports/github-issue-selection.json");
  const backlog = parseArtifactJson(files, "reports/review-backlog-selection.json");
  const gate = parseArtifactJson(files, "reports/triage-gate.json");
  let sourceKind: SentinelRecoverySourceKind = "triage";
  let sourceId = runId;
  let sourceRevision = baseSha;
  if (
    issue && typeof issue.issue_id === "number" && positiveSafeInteger(issue.issue_id) && validSha256(issue.fingerprint)
  ) {
    sourceKind = "github_issue";
    sourceId = String(issue.issue_id);
    sourceRevision = issue.fingerprint;
  } else if (backlog && validSha256(backlog.fingerprint)) {
    sourceKind = "review_backlog";
    sourceId = backlog.fingerprint;
    sourceRevision = typeof backlog.affected_sha === "string" && FULL_SHA.test(backlog.affected_sha)
      ? backlog.affected_sha
      : backlog.fingerprint;
  } else if (gate?.work_source === "incident") {
    sourceKind = "incident";
  }
  const record = parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository: Deno.env.get("GITHUB_REPOSITORY") ?? "",
      source_kind: sourceKind,
      source_id: sourceId,
      source_revision: sourceRevision,
      candidate_generation: 1,
    },
    run_id: runId,
    attempt: 1,
    lease_token: `artifact-${artifact.id}`,
    base_sha: baseSha,
    phase: "recovery_pending",
    disposition: "active",
    state_version: 1,
    created_at: startedAt,
    updated_at: startedAt,
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: [artifact.id],
    artifact_digests: [encryptedDigest],
    reason: "encrypted candidate awaiting recovery",
    next_action: "reconstruct the exact base and candidate patch",
    predecessor: null,
  });
  return record;
};

/**
 * Authenticate the matrix cell report embedded in an already decrypted
 * artifact: exactly one cell report, valid report schema, and a matching
 * report digest. It returns the authenticated report or null; the result is
 * independent of retry evidence, so a normal terminal cell report can be
 * recognized without any candidate manifest or recovery record.
 */
export const authenticatedMatrixCellReport = async (
  files: readonly SentinelArtifactFile[],
): Promise<MatrixCellReportV1 | null> => {
  const cellReports = files.filter((file) => MATRIX_CELL_REPORT_PATH.test(file.path));
  if (cellReports.length !== 1) return null;
  try {
    const report = parseMatrixCellReportV1(await decodeJson(cellReports[0]!.bytes, "artifact_invalid"));
    await assertMatrixCellReportDigest(report);
    return report;
  } catch {
    return null;
  }
};

/**
 * Recognize authenticated matrix-cell retry evidence. The cell runner persists
 * a durable recovery identity and the candidate snapshot next to its report;
 * this function authenticates that evidence into the exact record the recovery
 * ledger is expected to own. The report digest, plan digest, immutable cell
 * contract, base, run attempt/branch, work-selection identity, and exact
 * changed paths are all bound together; authenticated encryption alone is not
 * treated as proof. Any inconsistency fails closed to `null` so the legacy
 * classification path keeps the artifact owner-visible instead of mutating Git.
 */
export const matrixCellRecoveryRecordFromArtifact = async (
  files: readonly SentinelArtifactFile[],
  repository: string,
): Promise<SentinelRecoveryRecordV1 | null> => {
  if (!SAFE_REPOSITORY.test(repository)) return null;
  const report = await authenticatedMatrixCellReport(files);
  if (report === null || report.status !== "retry_pending") return null;
  const cellReportFile = files.find((file) => MATRIX_CELL_REPORT_PATH.test(file.path))!;
  const manifestDirectory = cellReportFile.path.slice(0, cellReportFile.path.lastIndexOf("/"));
  const recordFile = files.find((file) =>
    file.path === `${manifestDirectory}/recovery-record.json` &&
    MATRIX_CELL_RECOVERY_RECORD_PATH.test(file.path)
  );
  if (!recordFile) return null;
  let record: SentinelRecoveryRecordV1;
  try {
    record = parseSentinelRecoveryRecord(decodeJson(recordFile.bytes, "artifact_invalid"));
  } catch {
    return null;
  }
  if (record.phase !== "recovery_pending" || record.identity.repository !== repository) return null;
  const manifestPath = `${manifestDirectory}/manifest.json`;
  if (!files.some((file) => file.path === manifestPath && CANDIDATE_MANIFEST.test(file.path))) return null;
  let snapshot: SentinelCandidateSnapshot;
  try {
    snapshot = parseSentinelCandidateSnapshot(files);
  } catch {
    return null;
  }
  if (snapshot.matrix === null) return null;
  const cell = snapshot.matrix.cellContract;
  const selectionFile = files.find((file) =>
    file.path === `${manifestDirectory}/work-selection.json` && MATRIX_CELL_WORK_SELECTION_PATH.test(file.path)
  );
  let workSelection: MatrixCellWorkSelectionV1 | null = null;
  if (selectionFile) {
    try {
      workSelection = parseMatrixCellWorkSelection(decodeJson(selectionFile.bytes, "artifact_invalid"));
    } catch {
      return null;
    }
    if (workSelection === null) return null;
  }
  const sourceRevision = await matrixCellRecoverySourceRevision(report.base_sha, cell, workSelection);
  if (
    record.identity.source_kind !== (workSelection?.source_kind ?? "triage") ||
    record.identity.source_id !== (workSelection?.source_id ?? report.cell_id) ||
    record.identity.source_revision !== sourceRevision
  ) return null;
  if (
    record.run_id !== report.run_id || record.attempt !== report.run_attempt ||
    record.base_sha !== report.base_sha
  ) return null;
  try {
    assertMatrixRecordSnapshotBinding(record, snapshot.matrix);
  } catch {
    return null;
  }
  if (
    snapshot.matrix.planDigest !== report.plan_digest || snapshot.matrix.runId !== report.run_id ||
    snapshot.matrix.runAttempt !== report.run_attempt
  ) return null;
  if (cell.cell_id !== report.cell_id || cell.branch !== report.branch || cell.base_sha !== report.base_sha) {
    return null;
  }
  const reportFingerprints = report.finding_dispositions.map((item) => item.fingerprint).sort(comparePaths);
  if (JSON.stringify(reportFingerprints) !== JSON.stringify([...cell.finding_fingerprints].sort(comparePaths))) {
    return null;
  }
  try {
    assertMatrixSnapshotOwnership(cell, snapshot);
  } catch {
    return null;
  }
  const changed = [...report.changed_paths].sort(comparePaths);
  if (
    JSON.stringify([...record.changed_files].sort(comparePaths)) !== JSON.stringify(changed) ||
    JSON.stringify(snapshot.entries.map((entry) => entry.path).sort(comparePaths)) !== JSON.stringify(changed)
  ) return null;
  return record;
};

const parseMatrixCellReportV1 = (value: unknown): MatrixCellReportV1 => {
  assertMatrixCellReportV1(value);
  return value as MatrixCellReportV1;
};

/**
 * Retain later matrix evidence under an artifact-scoped identity when the
 * authoritative work record is already terminal. The terminal record is never
 * rewritten; the evidence stays owner-visible through the existing ledger
 * instead of being silently dropped.
 */
export const matrixEvidenceRetentionRecord = (
  record: SentinelRecoveryRecordV1,
  artifact: GitHubArtifact,
  encryptedDigest: string,
  predecessorKey: string,
): SentinelRecoveryRecordV1 | null => {
  if (
    !artifact.workflowRunId || !validSha256(encryptedDigest.replace(/^sha256:/u, "")) ||
    !validIsoTimestamp(artifact.createdAt)
  ) return null;
  return parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      ...record.identity,
      source_id: `${record.identity.source_id}:artifact:${artifact.id}`,
    },
    run_id: record.run_id,
    attempt: record.attempt,
    lease_token: `artifact-${artifact.id}`,
    base_sha: record.base_sha,
    phase: "manual_required",
    disposition: "manual_required",
    state_version: 1,
    created_at: artifact.createdAt,
    updated_at: artifact.createdAt,
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: "unrecoverable_evidence",
    failure_fingerprint: encryptedDigest.replace(/^sha256:/u, ""),
    artifact_ids: [artifact.id],
    artifact_digests: [encryptedDigest],
    reason: "later useful matrix evidence arrived for a terminal recovery record",
    next_action: "a repository owner must inspect the encrypted evidence and its exact base",
    predecessor: predecessorKey,
  });
};

/**
 * Resolve the candidate generation against the durable ledger instead of
 * trusting the provisional generation embedded in the evidence.
 *
 * An active record for the exact same cell identity keeps its generation, so
 * replaying the identical artifact resolves to the same record and never
 * allocates a second generation. A terminal single-lineage record (empty or
 * rejected) starts the next generation, and an unknown cell lineage
 * provisionally begins at one. The computed generation is only ever committed
 * through the ledger compare-and-swap write; a concurrent writer that wins the
 * CAS forces the loser to re-read the authoritative ledger and re-resolve
 * before retrying.
 */
export const resolveMatrixRecoveryGeneration = (
  records: readonly SentinelRecoveryRecordV1[],
  identity: SentinelRecoveryIdentityV1,
): number => {
  const sameIdentity = records.filter((candidate) =>
    candidate.identity.repository === identity.repository &&
    candidate.identity.source_kind === identity.source_kind &&
    candidate.identity.source_id === identity.source_id &&
    candidate.identity.source_revision === identity.source_revision
  );
  const active = sameIdentity.filter((candidate) => candidate.disposition === "active");
  if (active.length > 0) {
    return Math.max(...active.map((candidate) => candidate.identity.candidate_generation));
  }
  if (sameIdentity.length > 0) {
    return Math.max(...sameIdentity.map((candidate) => candidate.identity.candidate_generation)) + 1;
  }
  return 1;
};

/** Rebind the artifact's provisional generation to the authoritative ledger. */
const matrixRecordWithResolvedGeneration = (
  record: SentinelRecoveryRecordV1,
  records: readonly SentinelRecoveryRecordV1[],
): SentinelRecoveryRecordV1 => {
  const generation = resolveMatrixRecoveryGeneration(records, record.identity);
  if (generation === record.identity.candidate_generation) return record;
  return parseSentinelRecoveryRecord({
    ...record,
    identity: { ...record.identity, candidate_generation: generation },
  });
};

/**
 * Resolve the canonical parent generation with the exact rule the Sentinel
 * selection path uses (maximum over every record of the same repository,
 * source kind, and source id, plus one). It is consulted only after
 * `activeCanonicalParentRecord` found no compatible active parent, so a
 * concurrent or retried recovery for the same work item reuses the linked
 * parent instead of forking the work item into two generations.
 */
const resolveCanonicalParentGeneration = (
  records: readonly SentinelRecoveryRecordV1[],
  identity: Omit<SentinelRecoveryIdentityV1, "candidate_generation">,
): number =>
  records
    .filter((candidate) =>
      candidate.identity.repository === identity.repository &&
      candidate.identity.source_kind === identity.source_kind &&
      candidate.identity.source_id === identity.source_id
    )
    .reduce((maximum, candidate) => Math.max(maximum, candidate.identity.candidate_generation), 0) + 1;

/**
 * Canonical parent record for a recovered matrix cell checkpoint. It carries
 * the authoritative work-item revision that Sentinel selection matches while
 * the exact cell binding stays on the child record it points at through
 * `predecessor`. The candidate branch and SHA are the child's actual durable
 * refs, so the recovery controller can observe them through the linkage.
 */
export const buildMatrixCellParentRecoveryRecord = (
  child: SentinelRecoveryRecordV1,
  parentIdentity: SentinelRecoveryIdentityV1,
  now: string,
): SentinelRecoveryRecordV1 => {
  if (!SAFE_BRANCH.test(child.candidate_branch ?? "") || child.candidate_sha === null || child.tree_sha === null) {
    throw new CandidateSnapshotError("recovery_record_invalid");
  }
  return parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: parentIdentity,
    run_id: child.run_id,
    attempt: child.attempt,
    lease_token: `matrix-parent-${child.run_id}-${child.attempt}-${parentIdentity.source_id}`,
    base_sha: child.base_sha,
    phase: "checkpoint_durable",
    disposition: "active",
    state_version: 1,
    created_at: now,
    updated_at: now,
    candidate_branch: child.candidate_branch,
    candidate_sha: child.candidate_sha,
    changed_files: child.changed_files,
    tree_sha: child.tree_sha,
    failure_class: null,
    failure_fingerprint: null,
    artifact_ids: child.artifact_ids,
    artifact_digests: child.artifact_digests,
    reason: "A recovered matrix cell checkpoint is durable for the canonical work item.",
    next_action: "Validate the durable checkpoint and request human delivery review.",
    predecessor: sentinelRecoveryIdentityKey(child.identity),
  });
};

const matrixCellWorkSelectionFromFiles = (
  files: readonly SentinelArtifactFile[],
): MatrixCellWorkSelectionV1 | null => {
  const matrixReport = files.find((file) => MATRIX_CELL_WORK_SELECTION_PATH.test(file.path));
  if (!matrixReport) return null;
  try {
    return parseMatrixCellWorkSelection(decodeJson(matrixReport.bytes, "artifact_invalid"));
  } catch {
    return null;
  }
};

/**
 * Fetch the exact current `development` ref from GitHub for stale-base checks.
 * `githubFetch` takes the repository-scoped path exactly like its other call
 * sites: the real GitHub ref endpoint is
 * `GET /repos/{owner}/{repo}/git/ref/heads/development` (a repository-less
 * `/git/ref/...` request is the live HTTP 404 the recovery run observed). The
 * response must name the exact `refs/heads/development` commit identity; any
 * other ref, non-commit object, or non-full SHA fails closed.
 */
export const currentRecoveryDevelopmentHead = async (
  token: string,
  repository: string,
  fetcher: GitHubRequest,
): Promise<string> => {
  const value = await githubFetch(
    token,
    repository,
    `/repos/${repository}/git/ref/heads/development`,
    { method: "GET" },
    fetcher,
  );
  const outer = isRecord(value) ? value : null;
  const object = isRecord(outer?.object) ? outer.object : null;
  if (
    outer?.ref !== "refs/heads/development" || object?.type !== "commit" ||
    typeof object.sha !== "string" || !FULL_SHA.test(object.sha)
  ) {
    throw new Error("Sentinel recovery development head is invalid");
  }
  return object.sha;
};

export const manualRecoveryRecordForLegacyArtifact = (
  repository: string,
  artifact: GitHubArtifact,
  encryptedDigest: string,
): SentinelRecoveryRecordV1 | null => {
  if (
    !SAFE_REPOSITORY.test(repository) || !artifact.workflowRunId || !artifact.workflowRunHeadSha ||
    !FULL_SHA.test(artifact.workflowRunHeadSha) || !validSha256(encryptedDigest.replace(/^sha256:/u, "")) ||
    !validIsoTimestamp(artifact.createdAt)
  ) return null;
  return parseSentinelRecoveryRecord({
    schema_version: 1,
    identity: {
      repository,
      source_kind: "triage",
      source_id: `${artifact.workflowRunId}:artifact:${artifact.id}`,
      source_revision: artifact.workflowRunHeadSha,
      candidate_generation: 1,
    },
    run_id: String(artifact.workflowRunId),
    attempt: 1,
    lease_token: `artifact-${artifact.id}`,
    base_sha: artifact.workflowRunHeadSha,
    phase: "manual_required",
    disposition: "manual_required",
    state_version: 1,
    created_at: artifact.createdAt,
    updated_at: artifact.createdAt,
    candidate_branch: null,
    candidate_sha: null,
    changed_files: [],
    tree_sha: null,
    failure_class: "artifact_invalid",
    failure_fingerprint: encryptedDigest.replace(/^sha256:/u, ""),
    artifact_ids: [artifact.id],
    artifact_digests: [encryptedDigest],
    reason: "authenticated legacy evidence lacks a provable recovery record",
    next_action: "a repository owner must inspect the encrypted artifact and its exact workflow base",
    predecessor: null,
  });
};

export const terminalRecoveryRecordForLegacyArtifact = (
  repository: string,
  artifact: GitHubArtifact,
  encryptedDigest: string,
  disposition: "manual_required" | "rejected" | "delivered",
): SentinelRecoveryRecordV1 | null => {
  const base = manualRecoveryRecordForLegacyArtifact(repository, artifact, encryptedDigest);
  if (!base) return null;
  return parseSentinelRecoveryRecord({
    ...base,
    phase: disposition,
    disposition,
    reason: "authenticated legacy evidence contains a terminal disposition",
    next_action: disposition === "manual_required"
      ? "a repository owner must inspect the encrypted artifact and its exact workflow base"
      : null,
  });
};

const artifactCreatedAtMs = (artifact: GitHubArtifact): number => {
  const timestamp = Date.parse(artifact.createdAt);
  return Number.isFinite(timestamp) ? timestamp : 0;
};

/** Select the newest bounded set of Sentinel evidence artifacts. */
export const selectSentinelRecoveryArtifacts = (
  artifacts: readonly GitHubArtifact[],
  maximum = MAX_RECOVERY_ARTIFACTS,
  requiredArtifactIds: ReadonlySet<number> = new Set(),
): readonly GitHubArtifact[] => {
  if (!positiveSafeInteger(maximum)) throw new Error("Sentinel recovery artifact bound is invalid");
  if ([...requiredArtifactIds].some((artifactId) => !positiveSafeInteger(artifactId))) {
    throw new Error("Required Sentinel recovery artifact ID is invalid");
  }
  const eligible = artifacts
    .filter((artifact) => !artifact.expired && ARTIFACT_NAME.test(artifact.name))
    .sort((left, right) => {
      const createdAtDifference = artifactCreatedAtMs(right) - artifactCreatedAtMs(left);
      return createdAtDifference !== 0 ? createdAtDifference : right.id - left.id;
    });
  return [
    ...eligible.filter((artifact) => requiredArtifactIds.has(artifact.id)),
    ...eligible.filter((artifact) => !requiredArtifactIds.has(artifact.id)),
  ].slice(0, maximum);
};

const workflowRunForArtifact = async (
  files: readonly SentinelArtifactFile[],
  artifact: GitHubArtifact,
  github: GitHubActionsClient,
): Promise<Readonly<Pick<GitHubWorkflowRun, "status" | "conclusion">> | undefined> => {
  const cycle = parseArtifactJson(files, "reports/cycle.json");
  const status = typeof cycle?.status === "string" ? cycle.status : null;
  if (
    status !== null && INCOMPLETE_FAILURE_STATUSES.has(status) ||
    explicitIncompleteFailureEvidence(parseArtifactJson(files, "reports/failure.json")) ||
    cycle !== null && terminalRecordState(cycle) || terminalArtifactRecord(files)
  ) {
    return undefined;
  }
  const runId = artifact.workflowRunId;
  if (!positiveSafeInteger(runId)) return undefined;
  const run = await github.getWorkflowRun(runId);
  return { status: run.status, conclusion: run.conclusion };
};

export type SentinelEvidenceEnvelope = Readonly<{
  files: readonly SentinelArtifactFile[];
  encrypted_digest: string;
}>;

/**
 * Bounded evidence-envelope loader for retained run evidence: unzips the
 * artifact zip, authenticates the encrypted bytes digest, and decrypts with
 * the existing Sentinel artifact key. No new secret, env name, or CLI.
 */
export const decryptSentinelEvidenceEnvelope = async (
  input: Readonly<{ zip: Uint8Array; encodedArtifactKey: string; privateRoot: string }>,
): Promise<SentinelEvidenceEnvelope> => {
  const keyBytes = decodeSentinelArtifactKey(input.encodedArtifactKey);
  try {
    const encrypted = await unzipEnvelope(input.zip, input.privateRoot);
    return {
      files: await decryptSentinelArtifact(encrypted, keyBytes),
      encrypted_digest: await artifactDigest(encrypted),
    };
  } finally {
    keyBytes.fill(0);
  }
};

const unzipEnvelope = async (zip: Uint8Array, privateRoot: string): Promise<Uint8Array<ArrayBuffer>> => {
  if (zip.byteLength > MAX_ARTIFACT_ZIP_BYTES) throw new Error("Sentinel evidence archive exceeds the recovery limit");
  const zipPath = `${privateRoot}/evidence.zip`;
  await Deno.writeFile(zipPath, zip, { mode: 0o600 });
  try {
    const output = await new Deno.Command("unzip", {
      args: ["-p", zipPath, "sentinel-evidence-v1.json"],
      stdout: "piped",
      stderr: "piped",
    }).output();
    if (!output.success || output.stdout.byteLength === 0) throw new Error("Sentinel evidence envelope is unavailable");
    return output.stdout;
  } finally {
    await Deno.remove(zipPath).catch(() => undefined);
  }
};

const gitNetworkEnvironment = (token: string, repository: string): Readonly<Record<string, string>> => ({
  GITHUB_REPOSITORY: repository,
  GIT_CONFIG_COUNT: "1",
  GIT_CONFIG_KEY_0: "http.https://github.com/.extraheader",
  GIT_CONFIG_VALUE_0: `AUTHORIZATION: basic ${btoa(`x-access-token:${token}`)}`,
  GIT_TERMINAL_PROMPT: "0",
  GIT_ASKPASS: "/bin/false",
});

const publishRecoveryBranchToOrigin = async (
  input: Readonly<{
    checkout: string;
    branch: string;
    candidateSha: string;
    token: string;
    repository: string;
  }>,
): Promise<void> => {
  const env = gitNetworkEnvironment(input.token, input.repository);
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const remoteOutput = await runGit(
      input.checkout,
      ["ls-remote", "--heads", "origin", `refs/heads/${input.branch}`],
      env,
    );
    const remoteLines = TEXT_DECODER.decode(remoteOutput).trim().split("\n").filter(Boolean);
    if (remoteLines.length > 1) throw new Error("Sentinel recovery remote branch lookup is ambiguous");
    const remoteSha = remoteLines.length === 0 ? null : remoteLines[0]!.split("\t")[0] ?? null;
    if (remoteSha !== null && remoteSha !== input.candidateSha) {
      throw new Error("Sentinel recovery remote branch conflicts");
    }
    if (remoteSha === input.candidateSha) return;
    // The lease pins the exact remote ref state just observed: a concurrent
    // writer that moved the branch forces this push to fail instead of being
    // silently overwritten. A new branch uses the zero SHA, which only allows
    // the creation ref.
    try {
      await runGit(
        input.checkout,
        [
          "push",
          "--no-verify",
          `--force-with-lease=refs/heads/${input.branch}:${remoteSha ?? ZERO_SHA}`,
          "origin",
          `${input.candidateSha}:refs/heads/${input.branch}`,
        ],
        env,
      );
    } catch (error) {
      if (attempt === 0) continue; // concurrent publication; re-verify and converge
      throw error;
    }
    const confirmed = await runGit(
      input.checkout,
      ["ls-remote", "--heads", "origin", `refs/heads/${input.branch}`],
      env,
    );
    if (TEXT_DECODER.decode(confirmed).trim().split("\t")[0] !== input.candidateSha) {
      throw new Error("Sentinel recovery branch did not reach the expected SHA");
    }
    return;
  }
};

const safeSummary = (result: SentinelArtifactRecoveryResult): string => {
  const candidate = result.candidate_sha ?? "none";
  const branch = result.candidate_branch ?? "none";
  return `artifact_digest=${result.artifact_digest} disposition=${result.disposition} reason=${result.reason} candidate=${candidate} branch=${branch}`;
};

const MAX_LEDGER_CAS_RETRIES = 3;

const isLedgerConflictError = (error: unknown): boolean =>
  error instanceof Error &&
  /(?:compare-and-swap|already exists|lost its lease|HTTP (?:409|422)|(?:^|\D)(?:409|422)(?:\D|$))/u.test(
    error.message,
  );

const terminalMatrixResult = (
  digest: string,
  recoveryRecord: SentinelRecoveryRecordV1 | null,
): SentinelArtifactRecoveryResult => ({
  schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
  disposition: "manual_required",
  reason: "terminal_record",
  artifact_digest: digest,
  candidate_branch: null,
  candidate_sha: null,
  tree_sha: null,
  changed_files: [],
  recovery_record: recoveryRecord,
  draft_pull_request: null,
});

const staleHeadResult = (
  digest: string,
  recoveryRecord: SentinelRecoveryRecordV1,
): SentinelArtifactRecoveryResult => ({
  schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
  disposition: "manual_required",
  reason: "development_head_advanced",
  artifact_digest: digest,
  candidate_branch: null,
  candidate_sha: null,
  tree_sha: null,
  changed_files: [],
  recovery_record: recoveryRecord,
  draft_pull_request: null,
});

/**
 * Result for an authenticated retry_pending matrix cell with valid but empty
 * candidate evidence (`no_candidate_diff`): the original classified failure
 * was scheduled for a bounded retry by the retry circuit. The durable record
 * stays non-terminal (`retry_wait`/active) with the evaluated retry history
 * and decision; the recovery-classification reason stays bound to the result
 * so the operator still sees why no checkpoint was published.
 */
const retryPendingMatrixResult = (
  digest: string,
  recoveryRecord: SentinelRecoveryRecordV1,
): SentinelArtifactRecoveryResult => ({
  schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
  disposition: "retry_pending",
  reason: "retry_scheduled",
  artifact_digest: digest,
  candidate_branch: null,
  candidate_sha: null,
  tree_sha: null,
  changed_files: recoveryRecord.changed_files,
  recovery_record: recoveryRecord,
  draft_pull_request: null,
});

/** Deterministic replay answer for an artifact already recovered under this identity. */
const recoveredFromExistingRecord = (
  existing: SentinelRecoveryRecordV1,
  input: Readonly<{ repository: string; artifactId: number; encryptedDigest: string }>,
): SentinelArtifactRecoveryResult => ({
  schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
  disposition: "recovered",
  reason: "recovered",
  artifact_digest: input.encryptedDigest,
  candidate_branch: existing.candidate_branch,
  candidate_sha: existing.candidate_sha,
  tree_sha: existing.tree_sha,
  changed_files: existing.changed_files,
  recovery_record: existing,
  draft_pull_request: buildSentinelRecoveryDraftPullRequest({
    repository: input.repository,
    record: existing,
    candidateBranch: existing.candidate_branch!,
    candidateSha: existing.candidate_sha!,
    artifactId: input.artifactId,
  }),
});

const sameIdentityFamily = (left: SentinelRecoveryIdentityV1, right: SentinelRecoveryIdentityV1): boolean =>
  left.repository === right.repository && left.source_kind === right.source_kind &&
  left.source_id === right.source_id && left.source_revision === right.source_revision;

/**
 * Exact-artifact terminal replay: an artifact id or SHA-256 digest already
 * recorded on a terminal record of the same identity family is the identical
 * evidence replay. It returns that terminal record so the caller retains the
 * existing terminal record/result with zero branch/PR/ledger mutation. Only
 * genuinely new artifact evidence may allocate a fresh generation; the
 * candidate generation of the terminal record is never consulted, because
 * replaying the same evidence must not suddenly claim a new generation.
 */
const terminalReplayRecordForIdentityFamily = (
  records: readonly SentinelRecoveryRecordV1[],
  identity: SentinelRecoveryIdentityV1,
  artifactId: number,
  digest: string,
): SentinelRecoveryRecordV1 | null => {
  const matches = records.filter((record) =>
    record.disposition !== "active" && sameIdentityFamily(record.identity, identity) &&
    (record.artifact_ids.includes(artifactId) || record.artifact_digests.includes(digest))
  );
  if (matches.length === 0) return null;
  return matches.reduce((best, candidate) =>
    candidate.identity.candidate_generation > best.identity.candidate_generation ||
      (candidate.identity.candidate_generation === best.identity.candidate_generation &&
        candidate.state_version > best.state_version)
      ? candidate
      : best
  );
};

/**
 * Terminal replay lookup for matrix evidence: the exact child (cell) family
 * first, then the canonical (work-item) family, because a terminal canonical
 * record carries the same artifact ids/digests through the parent linkage.
 */
const terminalReplayRecordForMatrixArtifact = (
  records: readonly SentinelRecoveryRecordV1[],
  identity: SentinelRecoveryIdentityV1,
  artifactId: number,
  digest: string,
  workSelection: MatrixCellWorkSelectionV1 | null,
): SentinelRecoveryRecordV1 | null => {
  const childFamily = terminalReplayRecordForIdentityFamily(records, identity, artifactId, digest);
  if (childFamily !== null) return childFamily;
  const canonical = matrixCellCanonicalParentIdentity(identity, workSelection);
  return canonical === null ? null : terminalReplayRecordForIdentityFamily(records, canonical, artifactId, digest);
};

/**
 * Apply the bounded retry policy to the original classified cell failure
 * through the exact `applySentinelRetryPolicyToRecovery` semantics the
 * recovery controller uses, so the durable history and decision count each
 * original failure attempt exactly once. An unrecognized failure class stays
 * owner-visible on the record but never enters the retry circuit.
 */
const applyMatrixRetryPolicy = async (
  ledger: Readonly<{ retry_history: SentinelRetryAttemptHistory }>,
  record: SentinelRecoveryRecordV1,
  now: string,
): Promise<
  {
    applied: Awaited<ReturnType<typeof applySentinelRetryPolicyToRecovery>>;
  } | null
> => {
  if (record.failure_class === null || record.failure_fingerprint === null) return null;
  const key = sentinelRecoveryIdentityKey(record.identity);
  const history = ledger.retry_history.filter((attempt) => sentinelRecoveryIdentityKey(attempt.identity) === key)
    .slice(-8);
  try {
    return {
      applied: await applySentinelRetryPolicyToRecovery({
        record,
        failure: {
          failure_class: record.failure_class as SentinelFailureClass,
          failure_fingerprint: record.failure_fingerprint,
          phase: record.phase,
          signature: record.reason ?? record.failure_class,
        },
        history,
        now,
      }),
    };
  } catch {
    return null;
  }
};

const withEvaluatedRetry = (
  ledger: SentinelRecoveryLedgerSnapshot["ledger"],
  record: SentinelRecoveryRecordV1,
  evaluated: Readonly<{ decision: SentinelRetryDecision | null; history: SentinelRetryAttemptHistory }>,
): SentinelRecoveryLedgerSnapshot["ledger"] => {
  if (evaluated.decision === null) return ledger;
  const key = sentinelRecoveryIdentityKey(record.identity);
  return {
    ...ledger,
    retry_history: [
      ...ledger.retry_history.filter((attempt) => sentinelRecoveryIdentityKey(attempt.identity) !== key),
      ...evaluated.history,
    ],
    retry_decisions: [
      ...ledger.retry_decisions.filter((entry) => entry.identity_key !== key),
      { identity_key: key, decision: evaluated.decision },
    ],
  };
};

/**
 * Durable evidence attachment for a recovered matrix child and its linked
 * canonical parent: the complete artifact_ids/artifact_digests set is written
 * in place without changing identity, phase, or state version, because
 * evidence rows are data, not a recovery transition. The parent is matched
 * through the exact `predecessor` linkage, so a later exact replay of any
 * terminal-triggering artifact matches this record family before a generation
 * is allocated.
 */
const withDurableArtifactEvidence = (
  ledger: SentinelRecoveryLedgerSnapshot["ledger"],
  childKey: string,
  artifactIds: readonly number[],
  artifactDigests: readonly string[],
): SentinelRecoveryLedgerSnapshot["ledger"] => {
  const records = ledger.records.map((record) => {
    const linked = sentinelRecoveryIdentityKey(record.identity) === childKey || record.predecessor === childKey;
    if (
      !linked || record.artifact_ids.length === artifactIds.length &&
        record.artifact_digests.length === artifactDigests.length &&
        record.artifact_ids.every((value, index) => value === artifactIds[index]) &&
        record.artifact_digests.every((value, index) => value === artifactDigests[index])
    ) return record;
    return parseSentinelRecoveryRecord({ ...record, artifact_ids: artifactIds, artifact_digests: artifactDigests });
  });
  if (records.every((record, index) => record === ledger.records[index])) return ledger;
  return parseSentinelRecoveryLedger({ ...ledger, records });
};

type CanonicalParentIdentity = Omit<SentinelRecoveryIdentityV1, "candidate_generation">;

const matrixCanonicalParentMatches = (
  candidate: SentinelRecoveryRecordV1,
  parent: CanonicalParentIdentity,
): boolean =>
  candidate.identity.repository === parent.repository &&
  candidate.identity.source_kind === parent.source_kind &&
  candidate.identity.source_id === parent.source_id &&
  candidate.identity.source_revision === parent.source_revision;

/**
 * Find the single existing active canonical parent for a work item before any
 * new generation is allocated. The match is independent of candidate
 * generation, so a second interrupted cell for the same issue/backlog reuses
 * the parent that already links the work item instead of forking a competing
 * active parent. If multiple active parents somehow exist, the lowest
 * generation (the first linked lineage) wins deterministically.
 */
const activeCanonicalParentRecord = (
  records: readonly SentinelRecoveryRecordV1[],
  parent: CanonicalParentIdentity,
): SentinelRecoveryRecordV1 | null => {
  const matches = records.filter((candidate) =>
    candidate.disposition === "active" && candidate.candidate_branch !== null && candidate.candidate_sha !== null &&
    matrixCanonicalParentMatches(candidate, parent)
  );
  if (matches.length === 0) return null;
  return matches.reduce((best, candidate) =>
    candidate.identity.candidate_generation < best.identity.candidate_generation ||
      (candidate.identity.candidate_generation === best.identity.candidate_generation &&
        candidate.state_version > best.state_version)
      ? candidate
      : best
  );
};

/**
 * Merge one recovered child checkpoint's evidence into the reused canonical
 * parent without allocating a generation, changing phase, or touching state
 * version: evidence rows are data, not a recovery transition, matching
 * `withDurableArtifactEvidence`, and both cells' artifact evidence must stay
 * durable on the single parent so neither child's evidence is orphaned. The
 * parent keeps its winner's checkpoint refs and child linkage.
 */
const withMergedCanonicalParentEvidence = (
  ledger: SentinelRecoveryLedgerSnapshot["ledger"],
  parent: SentinelRecoveryRecordV1,
  artifactIds: readonly number[],
  artifactDigests: readonly string[],
): SentinelRecoveryLedgerSnapshot["ledger"] => {
  const ids = [...new Set([...parent.artifact_ids, ...artifactIds])].sort((left, right) => left - right);
  const digests = [...new Set([...parent.artifact_digests, ...artifactDigests])].sort();
  if (
    ids.length === parent.artifact_ids.length && digests.length === parent.artifact_digests.length &&
    ids.every((value, index) => value === parent.artifact_ids[index]) &&
    digests.every((value, index) => value === parent.artifact_digests[index])
  ) return ledger;
  const records = ledger.records.map((record) =>
    record === parent
      ? parseSentinelRecoveryRecord({ ...record, artifact_ids: ids, artifact_digests: digests })
      : record
  );
  return parseSentinelRecoveryLedger({ ...ledger, records });
};

/**
 * Fail closed when the exact `development` head moved after the candidate was
 * reconstructed: no branch is published and no draft PR is opened until the
 * artifact is re-recovered against the new base.
 */
const staleHeadManualRecord = async (
  record: SentinelRecoveryRecordV1,
  observed: string,
): Promise<SentinelRecoveryRecordV1> => {
  const fingerprint = await stableSentinelFailureFingerprint({
    identity: record.identity,
    failure_class: "stale_source",
    code: "development_head_advanced",
    phase: "checkpoint_publishing",
    signature: `development head advanced from ${record.base_sha} to ${observed}`,
  });
  return transitionRecord(record, {
    phase: "manual_required",
    disposition: "manual_required",
    candidate_branch: null,
    candidate_sha: null,
    tree_sha: null,
    changed_files: [],
    failure_class: "stale_source",
    failure_fingerprint: fingerprint,
    reason: "The Sentinel development head advanced after reconstruction; no publish happened.",
    next_action: "Recover the encrypted candidate against the exact new development base.",
  });
};

/**
 * Recover one authenticated matrix cell artifact with idempotent replay and
 * CAS-coupled state. The exact same artifact always resolves to the same
 * record, generation, branch, SHA, and draft PR without duplicating a ledger
 * mutation; a concurrent CAS winner forces a re-read and re-resolution before
 * the write is retried. The original classified cell failure is persisted into
 * the durable retry history/decisions before or with the recovery, and the
 * exact development head is re-verified immediately before branch publication
 * and draft PR creation.
 */
const recoverMatrixArtifactInActions = async (
  input: Readonly<{
    checkout: string;
    repository: string;
    token: string;
    stateFetcher: typeof fetch;
    fetcher: GitHubRequest | undefined;
    matrixRecord: SentinelRecoveryRecordV1;
    artifact: GitHubArtifact;
    encrypted: Uint8Array<ArrayBuffer>;
    keyBytes: Uint8Array<ArrayBuffer>;
    decrypted: readonly SentinelArtifactFile[];
    encryptedDigest: string;
    expectedBaseSha: string;
    snapshot: SentinelRecoveryLedgerSnapshot;
  }>,
): Promise<Readonly<{ result: SentinelArtifactRecoveryResult; snapshot: SentinelRecoveryLedgerSnapshot }>> => {
  const attempt = async (snapshot: SentinelRecoveryLedgerSnapshot) => {
    // Before resolving or allocating any generation: an exact artifact replay
    // of an already terminal record for the same canonical or child identity
    // retains that terminal record and performs zero mutation. Only genuinely
    // new artifact evidence may allocate a fresh generation.
    const terminalReplay = terminalReplayRecordForMatrixArtifact(
      snapshot.ledger.records,
      input.matrixRecord.identity,
      input.artifact.id,
      input.encryptedDigest,
      matrixCellWorkSelectionFromFiles(input.decrypted),
    );
    if (terminalReplay !== null) {
      return { result: terminalMatrixResult(input.encryptedDigest, terminalReplay), snapshot };
    }
    const child = matrixRecordWithResolvedGeneration(input.matrixRecord, snapshot.ledger.records);
    const childKey = sentinelRecoveryIdentityKey(child.identity);
    const existing =
      snapshot.ledger.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === childKey) ?? null;
    const now = new Date().toISOString();

    // A terminal record is never rewritten; later evidence is retained under an
    // artifact-scoped identity and remains owner-visible.
    if (existing && existing.disposition !== "active") {
      const retention = matrixEvidenceRetentionRecord(
        child,
        input.artifact,
        input.encryptedDigest,
        childKey,
      );
      let ledger = snapshot.ledger;
      if (
        retention &&
        !ledger.records.some((candidate) =>
          sentinelRecoveryIdentityKey(candidate.identity) === sentinelRecoveryIdentityKey(retention.identity)
        )
      ) {
        ledger = upsertSentinelRecoveryRecord(ledger, retention, null);
      }
      const written = ledger === snapshot.ledger ? snapshot : await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): retain terminal matrix evidence ${input.artifact.id}`,
      });
      return { result: terminalMatrixResult(input.encryptedDigest, retention), snapshot: written };
    }

    // Exact replay of an already recovered artifact: nothing changes on the
    // ledger, branch, or draft PR.
    if (
      existing && existing.candidate_sha !== null && existing.candidate_branch !== null &&
      existing.artifact_digests.includes(input.encryptedDigest)
    ) {
      return {
        result: recoveredFromExistingRecord(existing, {
          repository: input.repository,
          artifactId: input.artifact.id,
          encryptedDigest: input.encryptedDigest,
        }),
        snapshot,
      };
    }

    // A durable checkpoint already exists for this identity and a new later
    // attempt arrived: the checkpoint stays authoritative and is never
    // demoted. The incoming artifact id and digest are attached to the
    // record before the retry policy/history is applied and are written to
    // the same ledger CAS write, so the policy result and the linked
    // canonical parent always carry the complete artifact_ids/artifact_digests
    // set. An exact replay of a terminal-triggering artifact is then matched
    // before any generation allocation.
    if (existing && existing.candidate_sha !== null && existing.candidate_branch !== null) {
      const evidence = safeRecordWithArtifact(existing, input.encryptedDigest, input.artifact.id);
      const applied = await applyMatrixRetryPolicy(snapshot.ledger, evidence, now);
      if (applied !== null && applied.applied.decision.disposition !== "retry_wait") {
        const ledger = withDurableArtifactEvidence(
          withEvaluatedRetry(
            upsertSentinelRecoveryRecord(snapshot.ledger, applied.applied.after, existing.state_version),
            applied.applied.after,
            applied.applied,
          ),
          childKey,
          applied.applied.after.artifact_ids,
          applied.applied.after.artifact_digests,
        );
        const written = await writeGitHubSentinelRecoveryLedger({
          token: input.token,
          repository: input.repository,
          fetcher: input.stateFetcher,
          snapshot,
          ledger,
          message: `chore(sentinel): terminate retry circuit ${childKey}`,
        });
        return { result: terminalMatrixResult(input.encryptedDigest, applied.applied.after), snapshot: written };
      }
      const ledger = withDurableArtifactEvidence(
        applied === null ? snapshot.ledger : withEvaluatedRetry(snapshot.ledger, evidence, applied.applied),
        childKey,
        evidence.artifact_ids,
        evidence.artifact_digests,
      );
      if (ledger === snapshot.ledger) {
        return {
          result: recoveredFromExistingRecord(existing, {
            repository: input.repository,
            artifactId: input.artifact.id,
            encryptedDigest: input.encryptedDigest,
          }),
          snapshot,
        };
      }
      const written = await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): record matrix failure ${childKey}`,
      });
      return {
        result: recoveredFromExistingRecord(evidence, {
          repository: input.repository,
          artifactId: input.artifact.id,
          encryptedDigest: input.encryptedDigest,
        }),
        snapshot: written,
      };
    }

    // An exact replay of an already scheduled retry (the same artifact
    // evidence recorded on an active retry_wait record) is not a new failure
    // attempt: only genuinely new cell evidence may advance the bounded retry
    // circuit. Zero ledger, branch, or pull-request mutation happens.
    if (
      existing && existing.disposition === "active" && existing.phase === "retry_wait" &&
      (existing.artifact_ids.includes(input.artifact.id) ||
        existing.artifact_digests.includes(input.encryptedDigest))
    ) {
      return { result: retryPendingMatrixResult(input.encryptedDigest, existing), snapshot };
    }

    // Fresh or interrupted identity: recover the candidate with the policy
    // outcome applied before any publication.
    let workingLedger = snapshot.ledger;
    let record = existing ?? child;
    if (
      existing &&
      (existing.phase === "claimed" || existing.phase === "implementation_running" ||
        existing.phase === "workspace_dirty" || existing.phase === "checkpoint_publishing" ||
        existing.phase === "checkpoint_durable" || existing.phase === "retry_wait")
    ) {
      record = transitionRecord(existing, {
        phase: "recovery_pending",
        disposition: "active",
        reason: "Encrypted evidence is available for the interrupted candidate.",
        next_action: "Reconstruct the candidate from authenticated ciphertext.",
      });
      workingLedger = upsertSentinelRecoveryRecord(workingLedger, record, existing.state_version);
    }
    if (record.phase !== "recovery_pending") {
      const retention = matrixEvidenceRetentionRecord(
        child,
        input.artifact,
        input.encryptedDigest,
        childKey,
      );
      let ledger = workingLedger;
      if (
        retention &&
        !ledger.records.some((candidate) =>
          sentinelRecoveryIdentityKey(candidate.identity) === sentinelRecoveryIdentityKey(retention.identity)
        )
      ) {
        ledger = upsertSentinelRecoveryRecord(ledger, retention, null);
      }
      const written = ledger === workingLedger ? snapshot : await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): retain non-recoverable matrix evidence ${input.artifact.id}`,
      });
      return { result: terminalMatrixResult(input.encryptedDigest, retention), snapshot: written };
    }

    const recordWithEvidence = safeRecordWithArtifact(record, input.encryptedDigest, input.artifact.id);
    const applied = await applyMatrixRetryPolicy(snapshot.ledger, recordWithEvidence, now);
    if (applied !== null && applied.applied.decision.disposition !== "retry_wait") {
      // The terminal record is written first so the retry metadata has a
      // matching record identity in the same ledger CAS write.
      const childLedger = upsertSentinelRecoveryRecord(
        workingLedger,
        applied.applied.after,
        existing === null ? null : record.state_version,
      );
      const ledger = withEvaluatedRetry(childLedger, applied.applied.after, applied.applied);
      const written = await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): terminate retry circuit ${childKey}`,
      });
      return { result: terminalMatrixResult(input.encryptedDigest, applied.applied.after), snapshot: written };
    }

    const result = await recoverSentinelArtifactCandidate({
      checkout: input.checkout,
      encryptedBytes: input.encrypted,
      keyBytes: input.keyBytes,
      record,
      expectedBaseSha: input.expectedBaseSha,
      artifactId: input.artifact.id,
    });
    if (result.disposition !== "recovered" || !result.recovery_record) {
      // A retry_pending cell with authenticated but empty candidate evidence
      // (`no_candidate_diff`) is a valid durable retry publication: the
      // bounded retry circuit already scheduled another attempt for the
      // original classified cell failure. The retry_wait record and its
      // evaluated history/decision must stay durable and non-terminal so the
      // next selection can reclaim it; terminalizing it here would silently
      // drop the scheduled retry. Every other classification — corrupt
      // ciphertext, wrong or unavailable base, invalid record binding, or
      // failed reconstruction — is invalid evidence and fails closed to
      // manual_required even though the retry circuit would permit a retry.
      if (
        applied !== null && applied.applied.decision.disposition === "retry_wait" &&
        result.reason === "no_candidate_diff"
      ) {
        const retryChildLedger = upsertSentinelRecoveryRecord(
          workingLedger,
          applied.applied.after,
          existing === null ? null : record.state_version,
        );
        const retryLedger = withDurableArtifactEvidence(
          withEvaluatedRetry(retryChildLedger, applied.applied.after, applied.applied),
          childKey,
          applied.applied.after.artifact_ids,
          applied.applied.after.artifact_digests,
        );
        const written = await writeGitHubSentinelRecoveryLedger({
          token: input.token,
          repository: input.repository,
          fetcher: input.stateFetcher,
          snapshot,
          ledger: retryLedger,
          message: `chore(sentinel): schedule retry ${childKey}`,
        });
        return {
          result: retryPendingMatrixResult(input.encryptedDigest, applied.applied.after),
          snapshot: written,
        };
      }
      const ledger = upsertSentinelRecoveryRecord(
        workingLedger,
        result.recovery_record!,
        existing === null ? null : record.state_version,
      );
      const written = await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): classify matrix artifact ${input.artifact.id}`,
      });
      return { result, snapshot: written };
    }
    const recovered = result.recovery_record;

    // Final current-head verification: the exact development head is re-read
    // immediately before branch publication and again before the draft PR.
    const publishHead = await currentRecoveryDevelopmentHead(input.token, input.repository, input.stateFetcher);
    if (publishHead !== recovered.base_sha) {
      const stale = await staleHeadManualRecord(record, publishHead);
      const ledger = upsertSentinelRecoveryRecord(
        workingLedger,
        stale,
        existing === null ? null : record.state_version,
      );
      const written = await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): stale published head ${childKey}`,
      });
      return {
        result: staleHeadResult(input.encryptedDigest, stale),
        snapshot: written,
      };
    }
    await publishRecoveryBranchToOrigin({
      checkout: input.checkout,
      branch: result.candidate_branch!,
      candidateSha: result.candidate_sha!,
      token: input.token,
      repository: input.repository,
    });
    const prHead = await currentRecoveryDevelopmentHead(input.token, input.repository, input.stateFetcher);
    if (prHead !== recovered.base_sha) {
      const stale = await staleHeadManualRecord(record, prHead);
      const ledger = upsertSentinelRecoveryRecord(
        workingLedger,
        stale,
        existing === null ? null : record.state_version,
      );
      const written = await writeGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
        snapshot,
        ledger,
        message: `chore(sentinel): stale pull-request head ${childKey}`,
      });
      return {
        result: staleHeadResult(input.encryptedDigest, stale),
        snapshot: written,
      };
    }
    await createOrReuseSentinelRecoveryDraftPullRequest({
      token: input.token,
      request: result.draft_pull_request!,
      fetcher: input.fetcher,
    });

    // Canonical parent linkage: the authoritative work-item revision is
    // advanced through CAS so the next Sentinel selection sees the recovered
    // checkpoint while the exact cell binding stays on the child record. The
    // existing compatible active canonical parent is found and reused first,
    // so a second interrupted cell for the same work item CAS-merges its
    // checkpoint evidence into that single parent instead of forking a
    // competing generation; a new generation is allocated only when no
    // compatible active parent exists.
    const childLedger = upsertSentinelRecoveryRecord(
      workingLedger,
      recovered,
      existing === null ? null : record.state_version,
    );
    let ledger = applied === null ? childLedger : withEvaluatedRetry(childLedger, recovered, applied.applied);
    const workSelection = matrixCellWorkSelectionFromFiles(input.decrypted);
    const parentBase = matrixCellCanonicalParentIdentity(recovered.identity, workSelection);
    if (parentBase !== null) {
      const existingParent = activeCanonicalParentRecord(ledger.records, parentBase);
      if (existingParent !== null) {
        ledger = withMergedCanonicalParentEvidence(
          ledger,
          existingParent,
          recovered.artifact_ids,
          recovered.artifact_digests,
        );
      } else {
        const parentIdentity = {
          ...parentBase,
          candidate_generation: resolveCanonicalParentGeneration(ledger.records, parentBase),
        };
        ledger = upsertSentinelRecoveryRecord(
          ledger,
          buildMatrixCellParentRecoveryRecord(recovered, parentIdentity, now),
          null,
        );
      }
    }
    const written = await writeGitHubSentinelRecoveryLedger({
      token: input.token,
      repository: input.repository,
      fetcher: input.stateFetcher,
      snapshot,
      ledger,
      message: `chore(sentinel): recover ${childKey}`,
    });
    return { result, snapshot: written };
  };

  let snapshot = input.snapshot;
  for (let retry = 0; retry < MAX_LEDGER_CAS_RETRIES; retry += 1) {
    try {
      return await attempt(snapshot);
    } catch (error) {
      if (!isLedgerConflictError(error)) throw error;
      snapshot = await readGitHubSentinelRecoveryLedger({
        token: input.token,
        repository: input.repository,
        fetcher: input.stateFetcher,
      });
    }
  }
  return {
    result: await manualResult(null, input.encryptedDigest, "ledger_conflict"),
    snapshot,
  };
};

/**
 * Actions entrypoint. It scans only the existing ciphertext evidence artifact
 * family, decrypts in memory, pushes a quarantined branch, and creates a draft
 * PR. Corrupt evidence is summarized categorically without printing errors or
 * decrypted content.
 */
export const recoverSentinelArtifactsInActions = async (
  input: Readonly<{
    checkout: string;
    repository: string;
    token: string;
    encodedArtifactKey: string;
    fetcher?: GitHubRequest;
  }>,
): Promise<readonly SentinelArtifactRecoveryResult[]> => {
  if (!SAFE_REPOSITORY.test(input.repository) || input.repository !== Deno.env.get("GITHUB_REPOSITORY")) {
    throw new Error("Sentinel recovery repository identity is invalid");
  }
  const keyBytes = decodeSentinelArtifactKey(input.encodedArtifactKey);
  let privateRoot: string | null = null;
  try {
    const github = new GitHubActionsClient({
      repository: input.repository,
      token: input.token,
      fetcher: input.fetcher,
    });
    const stateFetcher = (input.fetcher ?? fetch) as typeof fetch;
    let recoverySnapshot = await readGitHubSentinelRecoveryLedger({
      token: input.token,
      repository: input.repository,
      fetcher: stateFetcher,
    });
    const currentDevelopmentHead = await currentRecoveryDevelopmentHead(input.token, input.repository, stateFetcher);
    const requiredArtifactIds = new Set(
      recoverySnapshot.ledger.records
        .filter((record) => record.disposition === "active")
        .flatMap((record) => record.artifact_ids),
    );
    const artifacts = selectSentinelRecoveryArtifacts(
      await github.listRepositoryArtifacts({ createdAfterMs: Date.now() - 90 * 24 * 60 * 60 * 1_000 }),
      MAX_RECOVERY_ARTIFACTS,
      requiredArtifactIds,
    );
    const results: SentinelArtifactRecoveryResult[] = [];
    privateRoot = await Deno.makeTempDir({ prefix: "sentinel-artifact-recovery-private-" });
    for (const artifact of artifacts) {
      let zip: Uint8Array<ArrayBuffer> | null = null;
      let encrypted: Uint8Array<ArrayBuffer> | null = null;
      let encryptedDigest = "sha256:unknown";
      let decrypted: SentinelArtifactFile[] = [];
      try {
        const downloaded = await github.downloadArtifact(artifact.id, MAX_ARTIFACT_ZIP_BYTES);
        const envelopeZip = new Uint8Array(downloaded);
        zip = envelopeZip;
        encrypted = await unzipEnvelope(envelopeZip, privateRoot);
        encryptedDigest = await artifactDigest(encrypted);
        decrypted = await decryptSentinelArtifact(encrypted, keyBytes);
        const matrixRecord = await matrixCellRecoveryRecordFromArtifact(decrypted, input.repository);
        // A normal terminal matrix cell report (succeeded/failed/blocked) is
        // not recovery evidence: it never carries retry evidence or a recovery
        // record, so it is skipped with zero ledger mutation and no result
        // classification instead of being misclassified as a legacy
        // artifact_invalid. Only an authenticated non-retry report is skipped;
        // malformed retry evidence (or any report that fails authentication)
        // still fails closed through the legacy classification path.
        const matrixReport = matrixRecord === null ? await authenticatedMatrixCellReport(decrypted) : null;
        if (matrixReport !== null && matrixReport.status !== "retry_pending") continue;
        const workflowRun = matrixRecord === null
          ? await workflowRunForArtifact(decrypted, artifact, github)
          : undefined;
        const artifactRecord = recordFromArtifact(decrypted, artifact, encryptedDigest, workflowRun) ?? matrixRecord;
        if (!artifactRecord) {
          const terminalDisposition = legacyArtifactTerminalDisposition(decrypted) ??
            (legacyArtifactHasTerminalReport(decrypted) || terminalArtifactRecord(decrypted)
              ? "manual_required"
              : null);
          let legacyRecord = terminalDisposition !== null
            ? terminalRecoveryRecordForLegacyArtifact(
              input.repository,
              artifact,
              encryptedDigest,
              terminalDisposition,
            )
            : manualRecoveryRecordForLegacyArtifact(input.repository, artifact, encryptedDigest);
          if (legacyRecord) {
            const existingLegacyRecord = recoverySnapshot.ledger.records.find((candidate) =>
              sentinelRecoveryIdentityKey(candidate.identity) === sentinelRecoveryIdentityKey(legacyRecord!.identity)
            ) ?? null;
            if (existingLegacyRecord) {
              legacyRecord = existingLegacyRecord;
            } else {
              recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
                token: input.token,
                repository: input.repository,
                fetcher: stateFetcher,
                snapshot: recoverySnapshot,
                ledger: upsertSentinelRecoveryRecord(recoverySnapshot.ledger, legacyRecord, null),
                message: `chore(sentinel): classify legacy artifact ${artifact.id}`,
              });
            }
          }
          results.push({
            schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
            disposition: "manual_required",
            reason: "artifact_invalid",
            artifact_digest: encryptedDigest,
            candidate_branch: null,
            candidate_sha: null,
            tree_sha: null,
            changed_files: [],
            recovery_record: legacyRecord,
            draft_pull_request: null,
          });
          continue;
        }
        if (matrixRecord !== null) {
          const recovered = await recoverMatrixArtifactInActions({
            checkout: input.checkout,
            repository: input.repository,
            token: input.token,
            stateFetcher,
            fetcher: input.fetcher,
            matrixRecord,
            artifact,
            encrypted,
            keyBytes,
            decrypted,
            encryptedDigest,
            expectedBaseSha: currentDevelopmentHead,
            snapshot: recoverySnapshot,
          });
          recoverySnapshot = recovered.snapshot;
          results.push(recovered.result);
          continue;
        }
        const identityKey = sentinelRecoveryIdentityKey(artifactRecord.identity);
        const existingRecord = recoverySnapshot.ledger.records.find((candidate) =>
          sentinelRecoveryIdentityKey(candidate.identity) === identityKey
        ) ?? null;
        if (existingRecord && existingRecord.disposition !== "active") {
          // Exact artifact replay of an already terminal disposition: retain
          // the existing terminal record and perform zero branch/PR/ledger
          // mutation. Only genuinely new evidence gets the artifact-scoped
          // retention record.
          const exactReplay = terminalReplayRecordForIdentityFamily(
            recoverySnapshot.ledger.records,
            artifactRecord.identity,
            artifact.id,
            encryptedDigest,
          );
          if (exactReplay !== null) {
            results.push(terminalMatrixResult(encryptedDigest, exactReplay));
            continue;
          }
          const evidenceRecord = terminalRecoveryRecordForLegacyArtifact(
            input.repository,
            artifact,
            encryptedDigest,
            existingRecord.disposition,
          );
          if (evidenceRecord) {
            const evidenceKey = sentinelRecoveryIdentityKey(evidenceRecord.identity);
            if (
              !recoverySnapshot.ledger.records.some((candidate) =>
                sentinelRecoveryIdentityKey(candidate.identity) === evidenceKey
              )
            ) {
              recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
                token: input.token,
                repository: input.repository,
                fetcher: stateFetcher,
                snapshot: recoverySnapshot,
                ledger: upsertSentinelRecoveryRecord(recoverySnapshot.ledger, evidenceRecord, null),
                message: `chore(sentinel): retain terminal artifact ${artifact.id}`,
              });
            }
          }
          continue;
        }
        let workingLedger = recoverySnapshot.ledger;
        let record = existingRecord ?? artifactRecord;
        if (
          existingRecord &&
          (existingRecord.phase === "claimed" || existingRecord.phase === "implementation_running" ||
            existingRecord.phase === "workspace_dirty" || existingRecord.phase === "checkpoint_publishing" ||
            existingRecord.phase === "checkpoint_durable" || existingRecord.phase === "retry_wait")
        ) {
          record = transitionRecord(existingRecord, {
            phase: "recovery_pending",
            disposition: "active",
            reason: "Encrypted evidence is available for the interrupted candidate.",
            next_action: "Reconstruct the candidate from authenticated ciphertext.",
          });
          workingLedger = upsertSentinelRecoveryRecord(
            workingLedger,
            record,
            existingRecord.state_version,
          );
        }
        if (record.phase !== "recovery_pending") {
          const owningWorkflowRun = workflowRun ??
            (artifact.workflowRunId ? await github.getWorkflowRun(artifact.workflowRunId) : undefined);
          if (owningWorkflowRun?.status === "completed" && owningWorkflowRun.conclusion !== "success") {
            const evidenceRecord = manualRecoveryRecordForLegacyArtifact(input.repository, artifact, encryptedDigest);
            if (evidenceRecord) {
              const evidenceKey = sentinelRecoveryIdentityKey(evidenceRecord.identity);
              if (
                !recoverySnapshot.ledger.records.some((candidate) =>
                  sentinelRecoveryIdentityKey(candidate.identity) === evidenceKey
                )
              ) {
                recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
                  token: input.token,
                  repository: input.repository,
                  fetcher: stateFetcher,
                  snapshot: recoverySnapshot,
                  ledger: upsertSentinelRecoveryRecord(recoverySnapshot.ledger, evidenceRecord, null),
                  message: `chore(sentinel): terminalize failed artifact ${artifact.id}`,
                });
              }
            }
          }
          continue;
        }
        const result = await recoverSentinelArtifactCandidate({
          checkout: input.checkout,
          encryptedBytes: encrypted,
          keyBytes,
          record,
          expectedBaseSha: currentDevelopmentHead,
          artifactId: artifact.id,
        });
        if (
          result.disposition === "recovered" && result.candidate_branch && result.candidate_sha &&
          result.draft_pull_request && result.recovery_record
        ) {
          // Final current-head verification, matching the matrix path: the
          // exact development head is re-read immediately before candidate
          // branch publication and again before draft PR creation. A moved
          // head fails closed to stale_source/manual with no push or no PR as
          // appropriate.
          const publishHead = await currentRecoveryDevelopmentHead(input.token, input.repository, stateFetcher);
          if (publishHead !== result.recovery_record.base_sha) {
            const stale = await staleHeadManualRecord(record, publishHead);
            recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
              token: input.token,
              repository: input.repository,
              fetcher: stateFetcher,
              snapshot: recoverySnapshot,
              ledger: upsertSentinelRecoveryRecord(
                workingLedger,
                stale,
                existingRecord === null ? null : record.state_version,
              ),
              message: `chore(sentinel): stale published head ${identityKey}`,
            });
            results.push(staleHeadResult(encryptedDigest, stale));
            continue;
          }
          await publishRecoveryBranchToOrigin({
            checkout: input.checkout,
            branch: result.candidate_branch,
            candidateSha: result.candidate_sha,
            token: input.token,
            repository: input.repository,
          });
          const prHead = await currentRecoveryDevelopmentHead(input.token, input.repository, stateFetcher);
          if (prHead !== result.recovery_record.base_sha) {
            const stale = await staleHeadManualRecord(record, prHead);
            recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
              token: input.token,
              repository: input.repository,
              fetcher: stateFetcher,
              snapshot: recoverySnapshot,
              ledger: upsertSentinelRecoveryRecord(
                workingLedger,
                stale,
                existingRecord === null ? null : record.state_version,
              ),
              message: `chore(sentinel): stale pull-request head ${identityKey}`,
            });
            results.push(staleHeadResult(encryptedDigest, stale));
            continue;
          }
          await createOrReuseSentinelRecoveryDraftPullRequest({
            token: input.token,
            request: result.draft_pull_request,
            fetcher: input.fetcher,
          });
        }
        if (result.recovery_record) {
          recoverySnapshot = await writeGitHubSentinelRecoveryLedger({
            token: input.token,
            repository: input.repository,
            fetcher: stateFetcher,
            snapshot: recoverySnapshot,
            ledger: upsertSentinelRecoveryRecord(
              workingLedger,
              result.recovery_record,
              existingRecord === null ? null : record.state_version,
            ),
            message: `chore(sentinel): recover ${identityKey}`,
          });
        }
        results.push(result);
      } catch {
        results.push({
          schema_version: SENTINEL_ARTIFACT_RECOVERY_SCHEMA_VERSION,
          disposition: "manual_required",
          reason: "artifact_corrupt",
          artifact_digest: encryptedDigest,
          candidate_branch: null,
          candidate_sha: null,
          tree_sha: null,
          changed_files: [],
          recovery_record: null,
          draft_pull_request: null,
        });
      } finally {
        for (const file of decrypted) file.bytes.fill(0);
        encrypted?.fill(0);
        zip?.fill(0);
      }
    }
    return results;
  } finally {
    keyBytes.fill(0);
    if (privateRoot !== null) await Deno.remove(privateRoot, { recursive: true }).catch(() => undefined);
  }
};

if (import.meta.main) {
  if (Deno.env.get("GITHUB_ACTIONS") !== "true") {
    throw new Error("Sentinel artifact recovery may run only in GitHub Actions");
  }
  const repository = Deno.env.get("GITHUB_REPOSITORY")?.trim();
  const token = Deno.env.get("GITHUB_TOKEN")?.trim();
  const encodedKey = Deno.env.get("SENTINEL_ARTIFACT_KEY")?.trim();
  if (!repository || !token || !encodedKey) throw new Error("Sentinel artifact recovery environment is incomplete");
  const results = await recoverSentinelArtifactsInActions({
    checkout: Deno.cwd(),
    repository,
    token,
    encodedArtifactKey: encodedKey,
  });
  for (const result of results) console.log(safeSummary(result));
}
