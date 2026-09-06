import type { TriageFinding } from "./types.ts";

/**
 * The matrix wire contracts are deliberately kept in this module.  A matrix
 * job is an untrusted boundary: JSON is accepted only after the exact V1
 * shape, ownership, and identity checks below have passed.
 */

export const MATRIX_SCHEMA_VERSION = 1 as const;
export const MATRIX_MAX_PARALLELISM = 4 as const;

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const RUN_ID = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/u;
const FINDING_ID = /^[A-Za-z0-9][A-Za-z0-9._:/-]{0,255}$/u;
const CELL_ID = /^cell-[0-9a-f]{64}$/u;
const MAX_EVIDENCE_DIGESTS = 256;
const MAX_FINDINGS = 128;
const MAX_PATHS_PER_FINDING = 256;
const MAX_PATH_LENGTH = 512;
const MAX_REQUIREMENTS = 256;
const MAX_STRING_LENGTH = 4_096;

type JsonObject = Record<string, unknown>;

const isObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const sortedKeys = (value: JsonObject): string[] => Object.keys(value).sort();

const hasExactKeys = (value: JsonObject, expected: readonly string[]): boolean => {
  const actual = sortedKeys(value);
  const keys = [...expected].sort();
  return actual.length === keys.length && actual.every((key, index) => key === keys[index]);
};

const isSafePositiveInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const compareStrings = (left: string, right: string): number => left < right ? -1 : left > right ? 1 : 0;

const sortedUnique = (values: readonly string[]): string[] => [...new Set(values)].sort(compareStrings);

const assertString = (value: unknown, label: string, pattern?: RegExp): string => {
  if (typeof value !== "string" || value.length === 0 || value.length > MAX_STRING_LENGTH) {
    throw new Error(`${label} must be a non-empty string`);
  }
  const hasControlCharacter = [...value].some((character) => {
    const code = character.charCodeAt(0);
    return code <= 0x1f || code === 0x7f;
  });
  if (value !== value.trim() || hasControlCharacter) {
    throw new Error(`${label} contains invalid whitespace or control characters`);
  }
  if (pattern && !pattern.test(value)) throw new Error(`${label} has an invalid format`);
  return value;
};

const assertSha = (value: unknown, label: string, pattern: RegExp): string => {
  if (typeof value !== "string" || !pattern.test(value)) throw new Error(`${label} must be a lowercase digest`);
  return value;
};

const assertPath = (value: unknown, label: string): string => {
  if (typeof value !== "string" || value.length === 0 || value.length > MAX_PATH_LENGTH) {
    throw new Error(`${label} must be a relative repository path`);
  }
  const normalized = value.endsWith("/") ? value.slice(0, -1) : value;
  if (
    value !== value.trim() || value.startsWith("/") || value.includes("\\") || value.includes("\u0000") ||
    value.endsWith("//") || normalized.length === 0 ||
    normalized.split("/").some((segment) => segment.length === 0 || segment === "." || segment === "..")
  ) {
    throw new Error(`${label} must be a normalized relative repository path`);
  }
  return value;
};

/**
 * A directory scope entry is an explicit trailing-slash path; it covers
 * descendants at a path-component boundary (`src/foo/` never authorizes
 * `src/foobar.ts`). A non-slash entry is an exact file and equals only itself.
 * Repository root and traversal are impossible after assertPath normalization.
 */
export const matrixAllowedPathDirectory = (allowed: string): string =>
  allowed.endsWith("/") ? allowed.slice(0, -1) : allowed;

export const matrixAllowedPathCovers = (path: string, allowed: string): boolean =>
  allowed.endsWith("/") ? path.startsWith(allowed) : path === allowed;

const assertPathList = (value: unknown, label: string, maximum = MAX_PATHS_PER_FINDING): string[] => {
  if (!Array.isArray(value) || value.length > maximum || !value.every((item) => typeof item === "string")) {
    throw new Error(`${label} must be an array of repository paths`);
  }
  const paths = value.map((item, index) => assertPath(item, `${label}[${index}]`));
  if (new Set(paths).size !== paths.length) throw new Error(`${label} contains duplicate paths`);
  return [...paths].sort(compareStrings);
};

const assertStringList = (value: unknown, label: string, maximum = MAX_REQUIREMENTS): string[] => {
  if (!Array.isArray(value) || value.length > maximum || !value.every((item) => typeof item === "string")) {
    throw new Error(`${label} must be an array of strings`);
  }
  const strings = value.map((item, index) => assertString(item, `${label}[${index}]`));
  if (new Set(strings).size !== strings.length) throw new Error(`${label} contains duplicates`);
  return [...strings].sort(compareStrings);
};

const assertNoOverlap = (left: readonly string[], right: readonly string[], label: string): void => {
  for (const leftPath of left) {
    for (const rightPath of right) {
      if (!matrixPathsOverlap(leftPath, rightPath)) continue;
      // An explicit allowed directory ancestor of a prohibited descendant is a
      // permitted exclusion: the protected path stays in the manifest and the
      // per-file protected checks still reject every descendant. An allowed
      // exact protected file, an equal protected directory, or an allowed path
      // beneath a prohibited directory remain conflicts.
      if (
        leftPath.endsWith("/") &&
        matrixAllowedPathDirectory(rightPath).startsWith(`${matrixAllowedPathDirectory(leftPath)}/`)
      ) continue;
      throw new Error(`${label} contains an allowed/prohibited path overlap`);
    }
  }
};

/** Path overlap treats a directory and any path below it as the same write scope. */
export const matrixPathsOverlap = (left: string, right: string): boolean => {
  const leftDirectory = matrixAllowedPathDirectory(left);
  const rightDirectory = matrixAllowedPathDirectory(right);
  return leftDirectory === rightDirectory || leftDirectory.startsWith(`${rightDirectory}/`) ||
    rightDirectory.startsWith(`${leftDirectory}/`);
};

const anyPathOverlap = (left: readonly string[], right: readonly string[]): boolean =>
  left.some((leftPath) => right.some((rightPath) => matrixPathsOverlap(leftPath, rightPath)));

const assertFindingId = (value: unknown, label: string): string => assertString(value, label, FINDING_ID);

export type MatrixEvidenceDigestV1 = Readonly<{
  name: string;
  sha256: string;
}>;

export type MatrixEvidenceDigestInput = Readonly<{
  /** `path` is accepted as an input alias; emitted manifests always use `name`. */
  name?: string;
  path?: string;
  sha256: string;
}>;

/** Triage finding plus the explicit ownership data required before fan-out. */
export type MatrixFindingInput = Readonly<{
  id: string;
  fingerprint: string;
  allowed_paths: readonly string[];
  prohibited_paths?: readonly string[];
  shared_paths?: readonly string[];
  depends_on?: readonly string[];
  validation_requirements?: readonly string[];
  /** Non-actionable findings cannot be sent to an implementation cell. */
  actionable?: boolean;
}>;

export type MatrixFindingOwnershipV1 = Readonly<{
  finding_id: string;
  fingerprint: string;
  allowed_paths: readonly string[];
  prohibited_paths: readonly string[];
  shared_paths: readonly string[];
  depends_on: readonly string[];
  validation_requirements: readonly string[];
}>;

export type MatrixConflictReason = "allowed_path_overlap" | "shared_contract_overlap" | "dependency";

export type MatrixConflictEdgeV1 = Readonly<{
  left_finding_id: string;
  right_finding_id: string;
  reasons: readonly MatrixConflictReason[];
}>;

export type MatrixConflictGraphV1 = Readonly<{
  schema_version: 1;
  finding_ids: readonly string[];
  edges: readonly MatrixConflictEdgeV1[];
  components: readonly (readonly string[])[];
}>;

export type MatrixCellV1 = Readonly<{
  cell_id: string;
  finding_ids: readonly string[];
  finding_fingerprints: readonly string[];
  allowed_paths: readonly string[];
  prohibited_paths: readonly string[];
  shared_paths: readonly string[];
  dependencies: readonly string[];
  validation_requirements: readonly string[];
  base_sha: string;
  branch: string;
  report_path: string;
  artifact_name: string;
}>;

export type MatrixPlanV1 = Readonly<{
  schema_version: 1;
  run_id: string;
  run_attempt: number;
  base_sha: string;
  evidence_digests: readonly MatrixEvidenceDigestV1[];
  ownership: readonly MatrixFindingOwnershipV1[];
  maximum_parallelism: 4;
  cells: readonly MatrixCellV1[];
  manifest_digest: string;
}>;

export type MatrixFindingDispositionStatus =
  | "implemented"
  | "already_fixed"
  | "not_actionable"
  | "blocked";

export type MatrixCellFindingDispositionV1 = Readonly<{
  finding_id: string;
  fingerprint: string;
  status: MatrixFindingDispositionStatus;
  summary: string;
  changed_files: readonly string[];
  validation: readonly string[];
}>;

export type MatrixValidationCheckV1 = Readonly<{
  name: string;
  passed: boolean;
  detail: string;
}>;

export type MatrixValidationV1 = Readonly<{
  passed: boolean;
  checks: readonly MatrixValidationCheckV1[];
}>;

export type MatrixReplayOutcome = "improved" | "same_failure" | "regressed" | "unavailable" | "not_applicable";

export type MatrixReplayResultV1 = Readonly<{
  capture_fingerprint: string;
  attempted: boolean;
  outcome: MatrixReplayOutcome;
  detail: string;
}>;

export type MatrixReplayV1 = Readonly<{
  attempted: boolean;
  passed: boolean;
  results: readonly MatrixReplayResultV1[];
}>;

export type MatrixCellStatus = "succeeded" | "failed" | "retry_pending" | "blocked";

export type MatrixCellReportV1 = Readonly<{
  schema_version: 1;
  run_id: string;
  run_attempt: number;
  plan_digest: string;
  cell_id: string;
  base_sha: string;
  branch: string;
  head_sha: string | null;
  tree_sha: string | null;
  changed_paths: readonly string[];
  finding_dispositions: readonly MatrixCellFindingDispositionV1[];
  validation: MatrixValidationV1;
  replay: MatrixReplayV1;
  status: MatrixCellStatus;
  failure_reason: string | null;
  artifact_sha256: string | null;
  report_digest: string;
}>;

export type IntegrationCellDecisionStatus = "accept" | "reject" | "blocked";

export type IntegrationCellDecisionV1 = Readonly<{
  cell_id: string;
  decision: IntegrationCellDecisionStatus;
  reason: string;
  required_combined_checks: readonly string[];
  correction_paths: readonly string[];
}>;

export type IntegrationDecisionV1 = Readonly<{
  schema_version: 1;
  run_id: string;
  run_attempt: number;
  plan_digest: string;
  base_sha: string;
  decisions: readonly IntegrationCellDecisionV1[];
  combined_validation_requirements: readonly string[];
  correction_paths: readonly string[];
  summary: string;
  decision_digest: string;
}>;

export type MatrixCycleCellStatus = "accepted" | "rejected" | "blocked" | "failed" | "retry_pending";

export type MatrixCellDispositionV1 = Readonly<{
  cell_id: string;
  branch: string;
  finding_ids: readonly string[];
  status: MatrixCycleCellStatus;
  head_sha: string | null;
  reason: string | null;
}>;

export type MatrixAcceptedAncestryV1 = Readonly<{
  cell_id: string;
  cell_head_sha: string;
  integrated_head_sha: string;
  is_ancestor: boolean;
}>;

export type MatrixBranchDispositionV1 = Readonly<{
  cell_id: string;
  branch: string;
  head_sha: string | null;
  reason: string;
}>;

export type MatrixIntegratedCandidateV1 = Readonly<{
  base_sha: string;
  branch: string;
  head_sha: string;
  tree_sha: string;
}>;

export type MatrixDeliveryStatus = "not_attempted" | "ready" | "published" | "failed" | "rolled_back";

export type MatrixDeliveryOutcomeV1 = Readonly<{
  status: MatrixDeliveryStatus;
  pr_number: number | null;
  merge_sha: string | null;
  reason: string | null;
}>;

export type MatrixCycleReportV1 = Readonly<{
  schema_version: 1;
  run_id: string;
  run_attempt: number;
  plan_digest: string;
  base_sha: string;
  cell_dispositions: readonly MatrixCellDispositionV1[];
  accepted_ancestry: readonly MatrixAcceptedAncestryV1[];
  rejected_branches: readonly MatrixBranchDispositionV1[];
  blocked_branches: readonly MatrixBranchDispositionV1[];
  integrated_candidate: MatrixIntegratedCandidateV1 | null;
  delivery: MatrixDeliveryOutcomeV1;
  cycle_digest: string;
}>;

const normalizeEvidenceDigests = (
  value: readonly MatrixEvidenceDigestInput[],
): MatrixEvidenceDigestV1[] => {
  if (!Array.isArray(value) || value.length > MAX_EVIDENCE_DIGESTS) {
    throw new Error("Matrix evidence digests exceed the allowed limit");
  }
  const normalized = value.map((item, index) => {
    if (!isObject(item)) throw new Error(`evidence_digests[${index}] must be an object`);
    const name = item.name ?? item.path;
    if (item.name !== undefined && item.path !== undefined && item.name !== item.path) {
      throw new Error(`evidence_digests[${index}] name and path disagree`);
    }
    return {
      name: assertString(name, `evidence_digests[${index}].name`),
      sha256: assertSha(item.sha256, `evidence_digests[${index}].sha256`, SHA256),
    };
  });
  const names = normalized.map((item) => item.name);
  if (new Set(names).size !== names.length) throw new Error("Matrix evidence digest names must be unique");
  return normalized.sort((left, right) => compareStrings(left.name, right.name));
};

const normalizeFinding = (input: MatrixFindingInput, index: number): MatrixFindingOwnershipV1 => {
  if (!isObject(input)) throw new Error(`findings[${index}] must be an object`);
  const findingId = assertFindingId(input.id, `findings[${index}].id`);
  const fingerprint = assertSha(input.fingerprint, `findings[${index}].fingerprint`, SHA256);
  if (input.actionable === false) throw new Error(`Finding ${findingId} is not actionable`);
  const allowedPaths = assertPathList(input.allowed_paths, `findings[${index}].allowed_paths`);
  if (allowedPaths.length === 0) throw new Error(`Finding ${findingId} has no allowed paths`);
  const prohibitedPaths = assertPathList(input.prohibited_paths ?? [], `findings[${index}].prohibited_paths`);
  const sharedPaths = assertPathList(input.shared_paths ?? [], `findings[${index}].shared_paths`);
  const dependsOn = assertStringList(input.depends_on ?? [], `findings[${index}].depends_on`)
    .map((dependency) => assertFindingId(dependency, `findings[${index}].depends_on`));
  const validationRequirements = assertStringList(
    input.validation_requirements ?? [],
    `findings[${index}].validation_requirements`,
  );
  assertNoOverlap(allowedPaths, prohibitedPaths, `Finding ${findingId}`);
  return {
    finding_id: findingId,
    fingerprint,
    allowed_paths: allowedPaths,
    prohibited_paths: prohibitedPaths,
    shared_paths: sharedPaths,
    depends_on: dependsOn,
    validation_requirements: validationRequirements,
  };
};

/**
 * Canonical MatrixFindingInput normalization shared by plan building and the
 * convergence ownership comparison, so both sides apply identical validation,
 * sorting, and defaulting.
 */
export const normalizeFindings = (findings: readonly MatrixFindingInput[]): MatrixFindingOwnershipV1[] => {
  if (!Array.isArray(findings) || findings.length > MAX_FINDINGS) {
    throw new Error(`Matrix findings exceed the limit of ${MAX_FINDINGS}`);
  }
  const normalized = findings.map(normalizeFinding);
  const ids = new Set<string>();
  const fingerprints = new Set<string>();
  for (const finding of normalized) {
    if (ids.has(finding.finding_id)) throw new Error(`Duplicate matrix finding ID: ${finding.finding_id}`);
    if (fingerprints.has(finding.fingerprint)) {
      throw new Error(`Duplicate matrix finding fingerprint: ${finding.fingerprint}`);
    }
    ids.add(finding.finding_id);
    fingerprints.add(finding.fingerprint);
  }
  const knownIds = new Set(normalized.map((finding) => finding.finding_id));
  for (const finding of normalized) {
    for (const dependency of finding.depends_on) {
      if (!knownIds.has(dependency)) {
        throw new Error(`Finding ${finding.finding_id} depends on unknown finding ${dependency}`);
      }
      if (dependency === finding.finding_id) {
        throw new Error(`Finding ${finding.finding_id} cannot depend on itself`);
      }
    }
  }
  return normalized.sort((left, right) =>
    compareStrings(left.fingerprint, right.fingerprint) || compareStrings(left.finding_id, right.finding_id)
  );
};

const conflictReasons = (
  left: MatrixFindingOwnershipV1,
  right: MatrixFindingOwnershipV1,
): MatrixConflictReason[] => {
  const reasons: MatrixConflictReason[] = [];
  if (anyPathOverlap(left.allowed_paths, right.allowed_paths)) reasons.push("allowed_path_overlap");
  const leftContracts = [...left.shared_paths, ...left.allowed_paths];
  const rightContracts = [...right.shared_paths, ...right.allowed_paths];
  if (
    anyPathOverlap(left.shared_paths, rightContracts) ||
    anyPathOverlap(right.shared_paths, leftContracts)
  ) reasons.push("shared_contract_overlap");
  if (left.depends_on.includes(right.finding_id) || right.depends_on.includes(left.finding_id)) {
    reasons.push("dependency");
  }
  return reasons;
};

/** Build the deterministic conflict graph used to form isolated repair cells. */
export const buildMatrixConflictGraph = (
  findings: readonly MatrixFindingInput[],
): MatrixConflictGraphV1 => {
  const normalized = normalizeFindings(findings);
  const adjacency = new Map(normalized.map((finding) => [finding.finding_id, new Set<string>()]));
  const edges: MatrixConflictEdgeV1[] = [];
  for (let leftIndex = 0; leftIndex < normalized.length; leftIndex += 1) {
    const left = normalized[leftIndex]!;
    for (let rightIndex = leftIndex + 1; rightIndex < normalized.length; rightIndex += 1) {
      const right = normalized[rightIndex]!;
      const reasons = conflictReasons(left, right);
      if (reasons.length === 0) continue;
      edges.push({ left_finding_id: left.finding_id, right_finding_id: right.finding_id, reasons });
      adjacency.get(left.finding_id)!.add(right.finding_id);
      adjacency.get(right.finding_id)!.add(left.finding_id);
    }
  }
  const visited = new Set<string>();
  const components: string[][] = [];
  for (const finding of normalized) {
    if (visited.has(finding.finding_id)) continue;
    const component: string[] = [];
    const pending = [finding.finding_id];
    visited.add(finding.finding_id);
    while (pending.length > 0) {
      const current = pending.pop()!;
      component.push(current);
      for (const neighbor of adjacency.get(current) ?? []) {
        if (visited.has(neighbor)) continue;
        visited.add(neighbor);
        pending.push(neighbor);
      }
    }
    component.sort(compareStrings);
    components.push(component);
  }
  components.sort((left, right) => compareStrings(left[0]!, right[0]!));
  return {
    schema_version: 1,
    finding_ids: normalized.map((finding) => finding.finding_id),
    edges,
    components,
  };
};

const TEXT_ENCODER = new TextEncoder();

const encodeHex = (bytes: Uint8Array): string => {
  let output = "";
  for (const byte of bytes) output += byte.toString(16).padStart(2, "0");
  return output;
};

const sha256 = async (value: string): Promise<string> =>
  encodeHex(new Uint8Array(await crypto.subtle.digest("SHA-256", TEXT_ENCODER.encode(value))));

const canonicalize = (value: unknown, path = "$", seen = new Set<object>()): unknown => {
  if (value === null || typeof value === "string" || typeof value === "boolean") return value;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) throw new Error(`Non-finite number at ${path}`);
    return value;
  }
  if (typeof value !== "object") throw new Error(`Unsupported value at ${path}`);
  if (seen.has(value)) throw new Error(`Cyclic value at ${path}`);
  seen.add(value);
  try {
    if (Array.isArray(value)) return value.map((item, index) => canonicalize(item, `${path}[${index}]`, seen));
    const record = value as JsonObject;
    const output: JsonObject = {};
    for (const key of Object.keys(record).sort(compareStrings)) {
      const item = record[key];
      if (item === undefined) throw new Error(`Undefined value at ${path}.${key}`);
      output[key] = canonicalize(item, `${path}.${key}`, seen);
    }
    return output;
  } finally {
    seen.delete(value);
  }
};

/** Canonical JSON sorts object keys recursively while preserving array order. */
export const canonicalMatrixJson = (value: unknown): string => {
  const serialized = JSON.stringify(canonicalize(value));
  if (serialized === undefined) throw new Error("Matrix document cannot be serialized");
  return serialized;
};

const withoutDigest = (value: JsonObject, field: string): JsonObject => {
  const output: JsonObject = {};
  for (const key of Object.keys(value)) if (key !== field) output[key] = value[key];
  return output;
};

const digestDocument = (value: JsonObject, field: string): Promise<string> =>
  sha256(canonicalMatrixJson(withoutDigest(value, field)));

const matrixCellIdForFingerprints = async (fingerprints: readonly string[]): Promise<string> =>
  `cell-${await sha256(
    canonicalMatrixJson({ schema_version: 1, finding_fingerprints: [...fingerprints].sort(compareStrings) }),
  )}`;

/** Return the stable content-derived ID used by every matrix cell. */
export const matrixCellId = (fingerprints: readonly string[]): Promise<string> => {
  if (!Array.isArray(fingerprints) || fingerprints.length === 0) {
    throw new Error("A matrix cell must contain at least one finding fingerprint");
  }
  const normalized = fingerprints.map((fingerprint, index) =>
    assertSha(fingerprint, `finding_fingerprints[${index}]`, SHA256)
  );
  if (new Set(normalized).size !== normalized.length) throw new Error("Matrix cell fingerprints must be unique");
  return matrixCellIdForFingerprints(normalized);
};

export const matrixCellBranch = (runId: string, runAttempt: number, cellId: string): string => {
  assertString(runId, "run_id", RUN_ID);
  if (!isSafePositiveInteger(runAttempt)) throw new Error("run_attempt must be a positive integer");
  if (!CELL_ID.test(cellId)) throw new Error("cell_id has an invalid format");
  return `sentinel/candidate-${runId}-${runAttempt}-${cellId}`;
};

export const matrixCellReportPath = (runId: string, runAttempt: number, cellId: string): string => {
  assertString(runId, "run_id", RUN_ID);
  if (!isSafePositiveInteger(runAttempt)) throw new Error("run_attempt must be a positive integer");
  if (!CELL_ID.test(cellId)) throw new Error("cell_id has an invalid format");
  return `.sentinel/reports/matrix/${runId}-${runAttempt}/${cellId}.json`;
};

export const matrixCellArtifactName = (runId: string, runAttempt: number, cellId: string): string => {
  assertString(runId, "run_id", RUN_ID);
  if (!isSafePositiveInteger(runAttempt)) throw new Error("run_attempt must be a positive integer");
  if (!CELL_ID.test(cellId)) throw new Error("cell_id has an invalid format");
  return `sentinel-matrix-cell-v1-${runId}-${runAttempt}-${cellId}`;
};

export type MatrixPlanInput = Readonly<{
  run_id: string;
  run_attempt: number;
  base_sha: string;
  evidence_digests: readonly MatrixEvidenceDigestInput[];
  findings: readonly MatrixFindingInput[];
}>;

/**
 * Build and content-address a plan.  More than four independent components is
 * rejected instead of silently serializing or dropping a required finding.
 */
export const buildMatrixPlan = async (input: MatrixPlanInput): Promise<MatrixPlanV1> => {
  if (!isObject(input)) throw new Error("Matrix plan input must be an object");
  const runId = assertString(input.run_id, "run_id", RUN_ID);
  if (!isSafePositiveInteger(input.run_attempt)) throw new Error("run_attempt must be a positive integer");
  const baseSha = assertSha(input.base_sha, "base_sha", FULL_SHA);
  const evidenceDigests = normalizeEvidenceDigests(input.evidence_digests);
  const ownership = normalizeFindings(input.findings);
  const graph = buildMatrixConflictGraph(ownership.map((finding) => ({
    id: finding.finding_id,
    fingerprint: finding.fingerprint,
    allowed_paths: finding.allowed_paths,
    prohibited_paths: finding.prohibited_paths,
    shared_paths: finding.shared_paths,
    depends_on: finding.depends_on,
    validation_requirements: finding.validation_requirements,
  })));
  if (graph.components.length > MATRIX_MAX_PARALLELISM) {
    throw new Error(
      `Matrix requires ${graph.components.length} independent cells, exceeding maximum ${MATRIX_MAX_PARALLELISM}`,
    );
  }
  const byId = new Map(ownership.map((finding) => [finding.finding_id, finding]));
  const cells: MatrixCellV1[] = [];
  for (const component of graph.components) {
    const findings = component.map((findingId) => byId.get(findingId)!);
    const fingerprints = findings.map((finding) => finding.fingerprint).sort(compareStrings);
    const cellId = await matrixCellIdForFingerprints(fingerprints);
    const branch = matrixCellBranch(runId, input.run_attempt, cellId);
    cells.push({
      cell_id: cellId,
      finding_ids: [...component].sort(compareStrings),
      finding_fingerprints: fingerprints,
      allowed_paths: sortedUnique(findings.flatMap((finding) => finding.allowed_paths)),
      prohibited_paths: sortedUnique(findings.flatMap((finding) => finding.prohibited_paths)),
      shared_paths: sortedUnique(findings.flatMap((finding) => finding.shared_paths)),
      dependencies: [],
      validation_requirements: sortedUnique(findings.flatMap((finding) => finding.validation_requirements)),
      base_sha: baseSha,
      branch,
      report_path: matrixCellReportPath(runId, input.run_attempt, cellId),
      artifact_name: matrixCellArtifactName(runId, input.run_attempt, cellId),
    });
  }
  cells.sort((left, right) => compareStrings(left.cell_id, right.cell_id));
  const unsigned: Omit<MatrixPlanV1, "manifest_digest"> = {
    schema_version: 1,
    run_id: runId,
    run_attempt: input.run_attempt,
    base_sha: baseSha,
    evidence_digests: evidenceDigests,
    ownership,
    maximum_parallelism: MATRIX_MAX_PARALLELISM,
    cells,
  };
  const candidate = { ...unsigned, manifest_digest: "0".repeat(64) } as MatrixPlanV1;
  assertMatrixPlanV1(candidate);
  return { ...unsigned, manifest_digest: await matrixPlanDigest(candidate) };
};

const assertMatrixFindingOwnership = (value: unknown, label: string): MatrixFindingOwnershipV1 => {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "finding_id",
      "fingerprint",
      "allowed_paths",
      "prohibited_paths",
      "shared_paths",
      "depends_on",
      "validation_requirements",
    ])
  ) throw new Error(`${label} has an invalid ownership shape`);
  const findingId = assertFindingId(value.finding_id, `${label}.finding_id`);
  const fingerprint = assertSha(value.fingerprint, `${label}.fingerprint`, SHA256);
  const allowedPaths = assertPathList(value.allowed_paths, `${label}.allowed_paths`);
  if (allowedPaths.length === 0) throw new Error(`${label}.allowed_paths cannot be empty`);
  const prohibitedPaths = assertPathList(value.prohibited_paths, `${label}.prohibited_paths`);
  const sharedPaths = assertPathList(value.shared_paths, `${label}.shared_paths`);
  const dependsOn = assertStringList(value.depends_on, `${label}.depends_on`).map((dependency) =>
    assertFindingId(dependency, `${label}.depends_on`)
  );
  const validationRequirements = assertStringList(value.validation_requirements, `${label}.validation_requirements`);
  assertNoOverlap(allowedPaths, prohibitedPaths, label);
  return {
    finding_id: findingId,
    fingerprint,
    allowed_paths: allowedPaths,
    prohibited_paths: prohibitedPaths,
    shared_paths: sharedPaths,
    depends_on: dependsOn,
    validation_requirements: validationRequirements,
  };
};

const assertMatrixCellShape = (value: unknown, label: string): MatrixCellV1 => {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "cell_id",
      "finding_ids",
      "finding_fingerprints",
      "allowed_paths",
      "prohibited_paths",
      "shared_paths",
      "dependencies",
      "validation_requirements",
      "base_sha",
      "branch",
      "report_path",
      "artifact_name",
    ])
  ) throw new Error(`${label} has an invalid cell shape`);
  const cellId = assertString(value.cell_id, `${label}.cell_id`);
  if (!CELL_ID.test(cellId)) throw new Error(`${label}.cell_id has an invalid format`);
  const findingIds = assertStringList(value.finding_ids, `${label}.finding_ids`).map((findingId) =>
    assertFindingId(findingId, `${label}.finding_ids`)
  );
  if (findingIds.length === 0) throw new Error(`${label}.finding_ids cannot be empty`);
  const fingerprints = assertStringList(value.finding_fingerprints, `${label}.finding_fingerprints`).map((
    fingerprint,
  ) => assertSha(fingerprint, `${label}.finding_fingerprints`, SHA256));
  if (fingerprints.length !== findingIds.length) {
    throw new Error(`${label} finding IDs and fingerprints differ in count`);
  }
  const allowedPaths = assertPathList(value.allowed_paths, `${label}.allowed_paths`);
  if (allowedPaths.length === 0) throw new Error(`${label}.allowed_paths cannot be empty`);
  const prohibitedPaths = assertPathList(value.prohibited_paths, `${label}.prohibited_paths`);
  const sharedPaths = assertPathList(value.shared_paths, `${label}.shared_paths`);
  const dependencies = assertStringList(value.dependencies, `${label}.dependencies`).map((dependency) =>
    assertString(dependency, `${label}.dependencies`)
  );
  const validationRequirements = assertStringList(value.validation_requirements, `${label}.validation_requirements`);
  const baseSha = assertSha(value.base_sha, `${label}.base_sha`, FULL_SHA);
  const branch = assertString(value.branch, `${label}.branch`);
  if (!branch.startsWith("sentinel/candidate-")) throw new Error(`${label}.branch is not a Sentinel candidate branch`);
  const reportPath = assertPath(value.report_path, `${label}.report_path`);
  const artifactName = assertString(value.artifact_name, `${label}.artifact_name`);
  assertNoOverlap(allowedPaths, prohibitedPaths, label);
  return {
    cell_id: cellId,
    finding_ids: findingIds,
    finding_fingerprints: fingerprints,
    allowed_paths: allowedPaths,
    prohibited_paths: prohibitedPaths,
    shared_paths: sharedPaths,
    dependencies,
    validation_requirements: validationRequirements,
    base_sha: baseSha,
    branch,
    report_path: reportPath,
    artifact_name: artifactName,
  };
};

/** Structural validation for a decoded exact MatrixCellV1 contract. */
export const parseMatrixCellV1 = (value: unknown): MatrixCellV1 => assertMatrixCellShape(value, "Matrix cell");

const sameStrings = (left: readonly string[], right: readonly string[]): boolean =>
  left.length === right.length && left.every((value, index) => value === right[index]);

const assertPlanOwnershipAndGrouping = (plan: MatrixPlanV1): void => {
  const ownership = plan.ownership.map((finding, index) =>
    assertMatrixFindingOwnership(finding, `ownership[${index}]`)
  );
  const findingsById = new Map(ownership.map((finding) => [finding.finding_id, finding]));
  const cellsByFinding = new Map<string, MatrixCellV1>();
  for (const cell of plan.cells) {
    for (const findingId of cell.finding_ids) {
      if (cellsByFinding.has(findingId)) throw new Error(`Finding ${findingId} is assigned to multiple cells`);
      if (!findingsById.has(findingId)) throw new Error(`Cell references unknown finding ${findingId}`);
      cellsByFinding.set(findingId, cell);
    }
  }
  if (cellsByFinding.size !== ownership.length) throw new Error("Matrix plan does not cover every finding");
  const graph = buildMatrixConflictGraph(ownership.map((finding) => ({
    id: finding.finding_id,
    fingerprint: finding.fingerprint,
    allowed_paths: finding.allowed_paths,
    prohibited_paths: finding.prohibited_paths,
    shared_paths: finding.shared_paths,
    depends_on: finding.depends_on,
    validation_requirements: finding.validation_requirements,
  })));
  for (const edge of graph.edges) {
    if (cellsByFinding.get(edge.left_finding_id) !== cellsByFinding.get(edge.right_finding_id)) {
      throw new Error(
        `Conflicting findings ${edge.left_finding_id} and ${edge.right_finding_id} are split across cells`,
      );
    }
  }
  for (const component of graph.components) {
    const cell = cellsByFinding.get(component[0]!);
    if (!cell || !sameStrings([...component].sort(compareStrings), [...cell.finding_ids].sort(compareStrings))) {
      throw new Error("Matrix cells are not the deterministic conflict components");
    }
  }
  for (const cell of plan.cells) {
    const cellFindings = cell.finding_ids.map((findingId) => findingsById.get(findingId)!);
    const expectedFingerprints = cellFindings.map((finding) => finding.fingerprint).sort(compareStrings);
    if (!sameStrings(expectedFingerprints, cell.finding_fingerprints)) {
      throw new Error(`Cell ${cell.cell_id} finding fingerprints do not match ownership`);
    }
    const expectedAllowed = sortedUnique(cellFindings.flatMap((finding) => finding.allowed_paths));
    const expectedProhibited = sortedUnique(cellFindings.flatMap((finding) => finding.prohibited_paths));
    const expectedShared = sortedUnique(cellFindings.flatMap((finding) => finding.shared_paths));
    const expectedValidation = sortedUnique(cellFindings.flatMap((finding) => finding.validation_requirements));
    if (!sameStrings(expectedAllowed, cell.allowed_paths)) throw new Error(`Cell ${cell.cell_id} allowed paths differ`);
    if (!sameStrings(expectedProhibited, cell.prohibited_paths)) {
      throw new Error(`Cell ${cell.cell_id} prohibited paths differ`);
    }
    if (!sameStrings(expectedShared, cell.shared_paths)) throw new Error(`Cell ${cell.cell_id} shared paths differ`);
    if (!sameStrings(expectedValidation, cell.validation_requirements)) {
      throw new Error(`Cell ${cell.cell_id} validation requirements differ`);
    }
    if (cell.base_sha !== plan.base_sha) throw new Error(`Cell ${cell.cell_id} base SHA differs from plan`);
    if (cell.branch !== matrixCellBranch(plan.run_id, plan.run_attempt, cell.cell_id)) {
      throw new Error(`Cell ${cell.cell_id} branch is not deterministic`);
    }
    if (cell.report_path !== matrixCellReportPath(plan.run_id, plan.run_attempt, cell.cell_id)) {
      throw new Error(`Cell ${cell.cell_id} report path is not deterministic`);
    }
    if (cell.artifact_name !== matrixCellArtifactName(plan.run_id, plan.run_attempt, cell.cell_id)) {
      throw new Error(`Cell ${cell.cell_id} artifact name is not deterministic`);
    }
    for (const dependency of cell.dependencies) {
      if (!plan.cells.some((candidate) => candidate.cell_id === dependency)) {
        throw new Error(`Cell ${cell.cell_id} depends on unknown cell ${dependency}`);
      }
      if (dependency === cell.cell_id) throw new Error(`Cell ${cell.cell_id} cannot depend on itself`);
    }
  }
};

/** Synchronous structural and ownership validation for a decoded plan. */
export function assertMatrixPlanV1(value: unknown): asserts value is MatrixPlanV1 {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "schema_version",
      "run_id",
      "run_attempt",
      "base_sha",
      "evidence_digests",
      "ownership",
      "maximum_parallelism",
      "cells",
      "manifest_digest",
    ])
  ) throw new Error("Matrix plan has an invalid V1 shape");
  if (value.schema_version !== 1) throw new Error("Unknown MatrixPlan schema version");
  assertString(value.run_id, "run_id", RUN_ID);
  if (!isSafePositiveInteger(value.run_attempt)) throw new Error("run_attempt must be a positive integer");
  const baseSha = assertSha(value.base_sha, "base_sha", FULL_SHA);
  const evidence = normalizeEvidenceDigests(value.evidence_digests as MatrixEvidenceDigestInput[]);
  if (
    !sameStrings(
      evidence.map((item) => item.name),
      (value.evidence_digests as MatrixEvidenceDigestV1[]).map((item) => item.name),
    )
  ) {
    throw new Error("Matrix evidence digests must be in canonical order");
  }
  if (value.maximum_parallelism !== MATRIX_MAX_PARALLELISM) {
    throw new Error(`Matrix maximum_parallelism must remain ${MATRIX_MAX_PARALLELISM}`);
  }
  if (!Array.isArray(value.ownership) || value.ownership.length > MAX_FINDINGS) {
    throw new Error("Matrix ownership is invalid");
  }
  const ownership = value.ownership.map((finding, index) =>
    assertMatrixFindingOwnership(finding, `ownership[${index}]`)
  );
  if (!Array.isArray(value.cells) || value.cells.length > MATRIX_MAX_PARALLELISM) {
    throw new Error(`Matrix cells must not exceed ${MATRIX_MAX_PARALLELISM}`);
  }
  const cells = value.cells.map((cell, index) => assertMatrixCellShape(cell, `cells[${index}]`));
  if (!sameStrings(cells.map((cell) => cell.cell_id), [...cells.map((cell) => cell.cell_id)].sort(compareStrings))) {
    throw new Error("Matrix cells must be ordered by cell_id");
  }
  if (new Set(cells.map((cell) => cell.cell_id)).size !== cells.length) {
    throw new Error("Matrix cell IDs must be unique");
  }
  if (new Set(ownership.map((finding) => finding.finding_id)).size !== ownership.length) {
    throw new Error("Matrix finding IDs must be unique");
  }
  if (new Set(ownership.map((finding) => finding.fingerprint)).size !== ownership.length) {
    throw new Error("Matrix finding fingerprints must be unique");
  }
  if (baseSha !== value.base_sha) throw new Error("Matrix base SHA is invalid");
  assertPlanOwnershipAndGrouping({
    schema_version: 1,
    run_id: value.run_id as string,
    run_attempt: value.run_attempt as number,
    base_sha: value.base_sha as string,
    evidence_digests: evidence,
    ownership,
    maximum_parallelism: MATRIX_MAX_PARALLELISM,
    cells,
    manifest_digest: value.manifest_digest as string,
  });
  assertSha(value.manifest_digest, "manifest_digest", SHA256);
}

export const isMatrixPlanV1 = (value: unknown): value is MatrixPlanV1 => {
  try {
    assertMatrixPlanV1(value);
    return true;
  } catch {
    return false;
  }
};

const assertMatrixCellIds = async (plan: MatrixPlanV1): Promise<void> => {
  for (const cell of plan.cells) {
    const expected = await matrixCellId(cell.finding_fingerprints);
    if (cell.cell_id !== expected) throw new Error(`Cell ${cell.cell_id} is not derived from its findings`);
  }
};

export const matrixPlanDigest = (plan: MatrixPlanV1): Promise<string> => {
  assertMatrixPlanV1(plan);
  return assertMatrixCellIds(plan).then(() => digestDocument(plan as unknown as JsonObject, "manifest_digest"));
};

export const assertMatrixPlanDigest = async (plan: MatrixPlanV1): Promise<void> => {
  const expected = await matrixPlanDigest(plan);
  if (plan.manifest_digest !== expected) throw new Error("Matrix plan manifest digest does not match its content");
};

export const encodeMatrixPlanV1 = async (plan: MatrixPlanV1): Promise<string> => {
  await assertMatrixPlanDigest(plan);
  return `${canonicalMatrixJson(plan)}\n`;
};

export const parseMatrixPlanV1 = async (raw: string | Uint8Array): Promise<MatrixPlanV1> => {
  let parsed: unknown;
  try {
    parsed = JSON.parse(typeof raw === "string" ? raw : new TextDecoder("utf-8", { fatal: true }).decode(raw));
  } catch {
    throw new Error("Matrix plan JSON is invalid");
  }
  assertMatrixPlanV1(parsed);
  await assertMatrixPlanDigest(parsed);
  return parsed;
};

const assertStringOrNull = (value: unknown, label: string, pattern?: RegExp): string | null => {
  if (value === null) return null;
  return assertString(value, label, pattern);
};

const assertMatrixFindingDisposition = (value: unknown, label: string): MatrixCellFindingDispositionV1 => {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "finding_id",
      "fingerprint",
      "status",
      "summary",
      "changed_files",
      "validation",
    ])
  ) throw new Error(`${label} has an invalid finding disposition shape`);
  const findingId = assertFindingId(value.finding_id, `${label}.finding_id`);
  const fingerprint = assertSha(value.fingerprint, `${label}.fingerprint`, SHA256);
  if (
    value.status !== "implemented" && value.status !== "already_fixed" && value.status !== "not_actionable" &&
    value.status !== "blocked"
  ) throw new Error(`${label}.status is invalid`);
  const summary = assertString(value.summary, `${label}.summary`);
  const changedFiles = assertPathList(value.changed_files, `${label}.changed_files`);
  const validation = assertStringList(value.validation, `${label}.validation`);
  return { finding_id: findingId, fingerprint, status: value.status, summary, changed_files: changedFiles, validation };
};

const assertMatrixValidation = (value: unknown, label: string): MatrixValidationV1 => {
  if (
    !isObject(value) || !hasExactKeys(value, ["passed", "checks"]) || typeof value.passed !== "boolean" ||
    !Array.isArray(value.checks)
  ) {
    throw new Error(`${label} has an invalid validation shape`);
  }
  const checks = value.checks.map((check, index) => {
    if (!isObject(check) || !hasExactKeys(check, ["name", "passed", "detail"]) || typeof check.passed !== "boolean") {
      throw new Error(`${label}.checks[${index}] has an invalid shape`);
    }
    return {
      name: assertString(check.name, `${label}.checks[${index}].name`),
      passed: check.passed,
      detail: assertString(check.detail, `${label}.checks[${index}].detail`),
    };
  });
  if (value.passed && checks.some((check) => !check.passed)) throw new Error(`${label} passed with a failed check`);
  return { passed: value.passed, checks };
};

const assertMatrixReplay = (value: unknown, label: string): MatrixReplayV1 => {
  if (
    !isObject(value) || !hasExactKeys(value, ["attempted", "passed", "results"]) ||
    typeof value.attempted !== "boolean" || typeof value.passed !== "boolean" || !Array.isArray(value.results)
  ) {
    throw new Error(`${label} has an invalid replay shape`);
  }
  const results = value.results.map((result, index) => {
    if (
      !isObject(result) || !hasExactKeys(result, ["capture_fingerprint", "attempted", "outcome", "detail"]) ||
      typeof result.attempted !== "boolean"
    ) {
      throw new Error(`${label}.results[${index}] has an invalid shape`);
    }
    const outcome = result.outcome;
    if (
      outcome !== "improved" && outcome !== "same_failure" && outcome !== "regressed" && outcome !== "unavailable" &&
      outcome !== "not_applicable"
    ) {
      throw new Error(`${label}.results[${index}].outcome is invalid`);
    }
    return {
      capture_fingerprint: assertSha(
        result.capture_fingerprint,
        `${label}.results[${index}].capture_fingerprint`,
        SHA256,
      ),
      attempted: result.attempted,
      outcome: outcome as MatrixReplayOutcome,
      detail: assertString(result.detail, `${label}.results[${index}].detail`),
    };
  });
  if (value.passed && results.some((result) => result.outcome === "regressed")) {
    throw new Error(`${label} passed despite a regressed replay`);
  }
  if (!value.attempted && results.length > 0) throw new Error(`${label} has results while replay is not attempted`);
  return { attempted: value.attempted, passed: value.passed, results };
};

/** Validate a cell report, optionally binding it to the exact plan cell. */
export function assertMatrixCellReportV1(
  value: unknown,
  plan?: MatrixPlanV1,
): asserts value is MatrixCellReportV1 {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "schema_version",
      "run_id",
      "run_attempt",
      "plan_digest",
      "cell_id",
      "base_sha",
      "branch",
      "head_sha",
      "tree_sha",
      "changed_paths",
      "finding_dispositions",
      "validation",
      "replay",
      "status",
      "failure_reason",
      "artifact_sha256",
      "report_digest",
    ])
  ) throw new Error("Matrix cell report has an invalid V1 shape");
  if (value.schema_version !== 1) throw new Error("Unknown MatrixCellReport schema version");
  const runId = assertString(value.run_id, "run_id", RUN_ID);
  if (!isSafePositiveInteger(value.run_attempt)) throw new Error("run_attempt must be a positive integer");
  const planDigest = assertSha(value.plan_digest, "plan_digest", SHA256);
  const cellId = assertString(value.cell_id, "cell_id");
  if (!CELL_ID.test(cellId)) throw new Error("cell_id has an invalid format");
  const baseSha = assertSha(value.base_sha, "base_sha", FULL_SHA);
  const branch = assertString(value.branch, "branch");
  if (!branch.startsWith("sentinel/candidate-")) throw new Error("branch is not a Sentinel candidate branch");
  const headSha = assertStringOrNull(value.head_sha, "head_sha", FULL_SHA);
  const treeSha = assertStringOrNull(value.tree_sha, "tree_sha", FULL_SHA);
  const changedPaths = assertPathList(value.changed_paths, "changed_paths");
  if (!Array.isArray(value.finding_dispositions)) throw new Error("finding_dispositions must be an array");
  const findingDispositions = value.finding_dispositions.map((item, index) =>
    assertMatrixFindingDisposition(item, `finding_dispositions[${index}]`)
  );
  if (new Set(findingDispositions.map((item) => item.finding_id)).size !== findingDispositions.length) {
    throw new Error("finding_dispositions contains duplicate findings");
  }
  const validation = assertMatrixValidation(value.validation, "validation");
  assertMatrixReplay(value.replay, "replay");
  if (
    value.status !== "succeeded" && value.status !== "failed" && value.status !== "retry_pending" &&
    value.status !== "blocked"
  ) {
    throw new Error("Matrix cell report status is invalid");
  }
  const failureReason = assertStringOrNull(value.failure_reason, "failure_reason");
  assertStringOrNull(value.artifact_sha256, "artifact_sha256", SHA256);
  assertSha(value.report_digest, "report_digest", SHA256);
  if (value.status === "succeeded" && (headSha === null || treeSha === null || !validation.passed)) {
    throw new Error("A succeeded cell must have head/tree identity and passing validation");
  }
  if (value.status === "succeeded" && value.artifact_sha256 === null) {
    throw new Error("A succeeded cell must have an artifact digest");
  }
  if (value.status !== "succeeded" && failureReason === null) {
    throw new Error("A non-succeeded cell must record a failure reason");
  }
  if (plan) {
    assertMatrixPlanV1(plan);
    if (runId !== plan.run_id || value.run_attempt !== plan.run_attempt || baseSha !== plan.base_sha) {
      throw new Error("Matrix cell report identity differs from its plan");
    }
    if (planDigest !== plan.manifest_digest) throw new Error("Matrix cell report is bound to a different plan digest");
    const cell = plan.cells.find((candidate) => candidate.cell_id === cellId);
    if (!cell) throw new Error(`Matrix cell report references unknown cell ${cellId}`);
    if (branch !== cell.branch) throw new Error("Matrix cell report branch differs from its plan");
    const rawChangedPaths = value.changed_paths as readonly string[];
    if (!sameStrings(rawChangedPaths, [...rawChangedPaths].sort(compareStrings))) {
      throw new Error("changed_paths are not canonical");
    }
    if (changedPaths.some((path) => !cell.allowed_paths.some((allowed) => matrixPathsOverlap(path, allowed)))) {
      throw new Error(`Cell report changed a path outside cell ${cell.cell_id} ownership`);
    }
    const expectedFindingIds = [...cell.finding_ids].sort(compareStrings);
    const actualFindingIds = findingDispositions.map((item) => item.finding_id).sort(compareStrings);
    if (!sameStrings(expectedFindingIds, actualFindingIds)) {
      throw new Error("Cell report does not cover exactly its findings");
    }
    for (const disposition of findingDispositions) {
      const ownership = plan.ownership.find((finding) => finding.finding_id === disposition.finding_id)!;
      if (ownership.fingerprint !== disposition.fingerprint) throw new Error("Finding disposition fingerprint differs");
      if (
        disposition.changed_files.some((path) =>
          !cell.allowed_paths.some((allowed) => matrixPathsOverlap(path, allowed)) ||
          !changedPaths.some((changedPath) => matrixPathsOverlap(path, changedPath))
        )
      ) {
        throw new Error(`Finding ${disposition.finding_id} changed a path outside its cell ownership`);
      }
    }
    if (value.status === "succeeded" && findingDispositions.some((item) => item.status === "blocked")) {
      throw new Error("A succeeded cell cannot contain a blocked finding disposition");
    }
  }
}

export const isMatrixCellReportV1 = (value: unknown, plan?: MatrixPlanV1): value is MatrixCellReportV1 => {
  try {
    assertMatrixCellReportV1(value, plan);
    return true;
  } catch {
    return false;
  }
};

export const matrixCellReportDigest = (report: MatrixCellReportV1): Promise<string> => {
  assertMatrixCellReportV1(report);
  return digestDocument(report as unknown as JsonObject, "report_digest");
};

export const assertMatrixCellReportDigest = async (report: MatrixCellReportV1): Promise<void> => {
  const expected = await matrixCellReportDigest(report);
  if (report.report_digest !== expected) throw new Error("Matrix cell report digest does not match its content");
};

const assertIntegrationDecisionCell = (value: unknown, label: string): IntegrationCellDecisionV1 => {
  if (
    !isObject(value) ||
    !hasExactKeys(value, ["cell_id", "decision", "reason", "required_combined_checks", "correction_paths"])
  ) {
    throw new Error(`${label} has an invalid integration decision shape`);
  }
  const cellId = assertString(value.cell_id, `${label}.cell_id`);
  if (!CELL_ID.test(cellId)) throw new Error(`${label}.cell_id has an invalid format`);
  if (value.decision !== "accept" && value.decision !== "reject" && value.decision !== "blocked") {
    throw new Error(`${label}.decision is invalid`);
  }
  return {
    cell_id: cellId,
    decision: value.decision,
    reason: assertString(value.reason, `${label}.reason`),
    required_combined_checks: assertStringList(value.required_combined_checks, `${label}.required_combined_checks`),
    correction_paths: assertPathList(value.correction_paths, `${label}.correction_paths`),
  };
};

export function assertIntegrationDecisionV1(
  value: unknown,
  plan?: MatrixPlanV1,
): asserts value is IntegrationDecisionV1 {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "schema_version",
      "run_id",
      "run_attempt",
      "plan_digest",
      "base_sha",
      "decisions",
      "combined_validation_requirements",
      "correction_paths",
      "summary",
      "decision_digest",
    ])
  ) throw new Error("Integration decision has an invalid V1 shape");
  if (value.schema_version !== 1) throw new Error("Unknown IntegrationDecision schema version");
  const runId = assertString(value.run_id, "run_id", RUN_ID);
  if (!isSafePositiveInteger(value.run_attempt)) throw new Error("run_attempt must be a positive integer");
  const planDigest = assertSha(value.plan_digest, "plan_digest", SHA256);
  const baseSha = assertSha(value.base_sha, "base_sha", FULL_SHA);
  if (!Array.isArray(value.decisions)) throw new Error("decisions must be an array");
  const decisions = value.decisions.map((item, index) => assertIntegrationDecisionCell(item, `decisions[${index}]`));
  if (new Set(decisions.map((decision) => decision.cell_id)).size !== decisions.length) {
    throw new Error("Integration decisions contain duplicate cells");
  }
  assertStringList(
    value.combined_validation_requirements,
    "combined_validation_requirements",
  );
  assertPathList(value.correction_paths, "correction_paths");
  assertString(value.summary, "summary");
  assertSha(value.decision_digest, "decision_digest", SHA256);
  if (plan) {
    assertMatrixPlanV1(plan);
    if (runId !== plan.run_id || value.run_attempt !== plan.run_attempt || baseSha !== plan.base_sha) {
      throw new Error("Integration decision identity differs from its plan");
    }
    if (planDigest !== plan.manifest_digest) throw new Error("Integration decision is bound to a different plan");
    const expectedCells = plan.cells.map((cell) => cell.cell_id).sort(compareStrings);
    const actualCells = decisions.map((decision) => decision.cell_id).sort(compareStrings);
    if (!sameStrings(expectedCells, actualCells)) {
      throw new Error("Integration decision must cover every matrix cell exactly once");
    }
  }
}

export const isIntegrationDecisionV1 = (value: unknown, plan?: MatrixPlanV1): value is IntegrationDecisionV1 => {
  try {
    assertIntegrationDecisionV1(value, plan);
    return true;
  } catch {
    return false;
  }
};

export const integrationDecisionDigest = (decision: IntegrationDecisionV1): Promise<string> => {
  assertIntegrationDecisionV1(decision);
  return digestDocument(decision as unknown as JsonObject, "decision_digest");
};

export const assertIntegrationDecisionDigest = async (decision: IntegrationDecisionV1): Promise<void> => {
  const expected = await integrationDecisionDigest(decision);
  if (decision.decision_digest !== expected) throw new Error("Integration decision digest does not match its content");
};

const assertCycleCellDisposition = (value: unknown, label: string): MatrixCellDispositionV1 => {
  if (!isObject(value) || !hasExactKeys(value, ["cell_id", "branch", "finding_ids", "status", "head_sha", "reason"])) {
    throw new Error(`${label} has an invalid cell disposition shape`);
  }
  const cellId = assertString(value.cell_id, `${label}.cell_id`);
  if (!CELL_ID.test(cellId)) throw new Error(`${label}.cell_id has an invalid format`);
  const branch = assertString(value.branch, `${label}.branch`);
  if (!branch.startsWith("sentinel/candidate-")) throw new Error(`${label}.branch is invalid`);
  const findingIds = assertStringList(value.finding_ids, `${label}.finding_ids`).map((findingId) =>
    assertFindingId(findingId, `${label}.finding_ids`)
  );
  if (findingIds.length === 0) throw new Error(`${label}.finding_ids cannot be empty`);
  if (
    value.status !== "accepted" && value.status !== "rejected" && value.status !== "blocked" &&
    value.status !== "failed" && value.status !== "retry_pending"
  ) {
    throw new Error(`${label}.status is invalid`);
  }
  const headSha = assertStringOrNull(value.head_sha, `${label}.head_sha`, FULL_SHA);
  const reason = assertStringOrNull(value.reason, `${label}.reason`);
  if (value.status !== "accepted" && reason === null) throw new Error(`${label} must explain a non-accepted status`);
  if (value.status === "accepted" && headSha === null) throw new Error(`${label} accepted status requires head_sha`);
  return { cell_id: cellId, branch, finding_ids: findingIds, status: value.status, head_sha: headSha, reason };
};

const assertCycleBranch = (value: unknown, label: string): MatrixBranchDispositionV1 => {
  if (!isObject(value) || !hasExactKeys(value, ["cell_id", "branch", "head_sha", "reason"])) {
    throw new Error(`${label} has an invalid branch disposition shape`);
  }
  const cellId = assertString(value.cell_id, `${label}.cell_id`);
  if (!CELL_ID.test(cellId)) throw new Error(`${label}.cell_id has an invalid format`);
  const branch = assertString(value.branch, `${label}.branch`);
  if (!branch.startsWith("sentinel/candidate-")) throw new Error(`${label}.branch is invalid`);
  return {
    cell_id: cellId,
    branch,
    head_sha: assertStringOrNull(value.head_sha, `${label}.head_sha`, FULL_SHA),
    reason: assertString(value.reason, `${label}.reason`),
  };
};

const assertIntegratedCandidate = (value: unknown): MatrixIntegratedCandidateV1 | null => {
  if (value === null) return null;
  if (!isObject(value) || !hasExactKeys(value, ["base_sha", "branch", "head_sha", "tree_sha"])) {
    throw new Error("integrated_candidate has an invalid shape");
  }
  const branch = assertString(value.branch, "integrated_candidate.branch");
  if (!branch.startsWith("sentinel/")) throw new Error("integrated_candidate.branch is invalid");
  return {
    base_sha: assertSha(value.base_sha, "integrated_candidate.base_sha", FULL_SHA),
    branch,
    head_sha: assertSha(value.head_sha, "integrated_candidate.head_sha", FULL_SHA),
    tree_sha: assertSha(value.tree_sha, "integrated_candidate.tree_sha", FULL_SHA),
  };
};

const assertDelivery = (value: unknown): MatrixDeliveryOutcomeV1 => {
  if (!isObject(value) || !hasExactKeys(value, ["status", "pr_number", "merge_sha", "reason"])) {
    throw new Error("delivery has an invalid shape");
  }
  if (
    value.status !== "not_attempted" && value.status !== "ready" && value.status !== "published" &&
    value.status !== "failed" && value.status !== "rolled_back"
  ) {
    throw new Error("delivery.status is invalid");
  }
  if (!(value.pr_number === null || (isSafePositiveInteger(value.pr_number) && value.pr_number <= 2_147_483_647))) {
    throw new Error("delivery.pr_number is invalid");
  }
  const mergeSha = assertStringOrNull(value.merge_sha, "delivery.merge_sha", FULL_SHA);
  const reason = assertStringOrNull(value.reason, "delivery.reason");
  if ((value.status === "failed" || value.status === "rolled_back") && reason === null) {
    throw new Error("delivery failure must have a reason");
  }
  if (value.status === "published" && mergeSha === null) throw new Error("published delivery requires merge_sha");
  return { status: value.status, pr_number: value.pr_number, merge_sha: mergeSha, reason };
};

export function assertMatrixCycleReportV1(
  value: unknown,
  plan?: MatrixPlanV1,
): asserts value is MatrixCycleReportV1 {
  if (
    !isObject(value) || !hasExactKeys(value, [
      "schema_version",
      "run_id",
      "run_attempt",
      "plan_digest",
      "base_sha",
      "cell_dispositions",
      "accepted_ancestry",
      "rejected_branches",
      "blocked_branches",
      "integrated_candidate",
      "delivery",
      "cycle_digest",
    ])
  ) throw new Error("Matrix cycle report has an invalid V1 shape");
  if (value.schema_version !== 1) throw new Error("Unknown MatrixCycleReport schema version");
  const runId = assertString(value.run_id, "run_id", RUN_ID);
  if (!isSafePositiveInteger(value.run_attempt)) throw new Error("run_attempt must be a positive integer");
  const planDigest = assertSha(value.plan_digest, "plan_digest", SHA256);
  const baseSha = assertSha(value.base_sha, "base_sha", FULL_SHA);
  if (!Array.isArray(value.cell_dispositions)) throw new Error("cell_dispositions must be an array");
  const cellDispositions = value.cell_dispositions.map((item, index) =>
    assertCycleCellDisposition(item, `cell_dispositions[${index}]`)
  );
  if (new Set(cellDispositions.map((item) => item.cell_id)).size !== cellDispositions.length) {
    throw new Error("cell_dispositions contain duplicate cells");
  }
  if (!Array.isArray(value.accepted_ancestry)) throw new Error("accepted_ancestry must be an array");
  const acceptedAncestry = value.accepted_ancestry.map((item, index) => {
    if (
      !isObject(item) || !hasExactKeys(item, ["cell_id", "cell_head_sha", "integrated_head_sha", "is_ancestor"]) ||
      typeof item.is_ancestor !== "boolean"
    ) {
      throw new Error(`accepted_ancestry[${index}] has an invalid shape`);
    }
    return {
      cell_id: assertString(item.cell_id, `accepted_ancestry[${index}].cell_id`),
      cell_head_sha: assertSha(item.cell_head_sha, `accepted_ancestry[${index}].cell_head_sha`, FULL_SHA),
      integrated_head_sha: assertSha(
        item.integrated_head_sha,
        `accepted_ancestry[${index}].integrated_head_sha`,
        FULL_SHA,
      ),
      is_ancestor: item.is_ancestor,
    };
  });
  if (acceptedAncestry.some((item) => !item.is_ancestor)) throw new Error("Accepted ancestry contains a failed proof");
  if (!Array.isArray(value.rejected_branches) || !Array.isArray(value.blocked_branches)) {
    throw new Error("Matrix branch dispositions must be arrays");
  }
  value.rejected_branches.map((item, index) => assertCycleBranch(item, `rejected_branches[${index}]`));
  value.blocked_branches.map((item, index) => assertCycleBranch(item, `blocked_branches[${index}]`));
  const integratedCandidate = assertIntegratedCandidate(value.integrated_candidate);
  assertDelivery(value.delivery);
  assertSha(value.cycle_digest, "cycle_digest", SHA256);
  if (plan) {
    assertMatrixPlanV1(plan);
    if (runId !== plan.run_id || value.run_attempt !== plan.run_attempt || baseSha !== plan.base_sha) {
      throw new Error("Matrix cycle report identity differs from its plan");
    }
    if (planDigest !== plan.manifest_digest) throw new Error("Matrix cycle report is bound to a different plan");
    const expectedCells = plan.cells.map((cell) => cell.cell_id).sort(compareStrings);
    const actualCells = cellDispositions.map((item) => item.cell_id).sort(compareStrings);
    if (!sameStrings(expectedCells, actualCells)) {
      throw new Error("Matrix cycle report must cover every cell exactly once");
    }
    for (const disposition of cellDispositions) {
      const cell = plan.cells.find((candidate) => candidate.cell_id === disposition.cell_id)!;
      if (
        disposition.branch !== cell.branch ||
        !sameStrings([...disposition.finding_ids].sort(compareStrings), [...cell.finding_ids].sort(compareStrings))
      ) {
        throw new Error(`Cycle disposition for ${disposition.cell_id} differs from its plan`);
      }
    }
    const acceptedIds = new Set(
      cellDispositions.filter((item) => item.status === "accepted").map((item) => item.cell_id),
    );
    const ancestryIds = new Set(acceptedAncestry.map((item) => item.cell_id));
    if (acceptedIds.size !== ancestryIds.size || [...acceptedIds].some((cellId) => !ancestryIds.has(cellId))) {
      throw new Error("Accepted cells do not have complete ancestry evidence");
    }
    if (cellDispositions.some((item) => item.status === "accepted") && integratedCandidate === null) {
      throw new Error("Accepted cells require an integrated candidate identity");
    }
  }
}

export const isMatrixCycleReportV1 = (value: unknown, plan?: MatrixPlanV1): value is MatrixCycleReportV1 => {
  try {
    assertMatrixCycleReportV1(value, plan);
    return true;
  } catch {
    return false;
  }
};

export const matrixCycleReportDigest = (report: MatrixCycleReportV1): Promise<string> => {
  assertMatrixCycleReportV1(report);
  return digestDocument(report as unknown as JsonObject, "cycle_digest");
};

export const assertMatrixCycleReportDigest = async (report: MatrixCycleReportV1): Promise<void> => {
  const expected = await matrixCycleReportDigest(report);
  if (report.cycle_digest !== expected) throw new Error("Matrix cycle report digest does not match its content");
};

/** Guard useful to callers that have a parsed object but need digest proof. */
export const validateMatrixPlanV1 = async (plan: MatrixPlanV1): Promise<MatrixPlanV1> => {
  assertMatrixPlanV1(plan);
  await assertMatrixPlanDigest(plan);
  return plan;
};

export const validateMatrixCellReportV1 = async (
  report: MatrixCellReportV1,
  plan?: MatrixPlanV1,
): Promise<MatrixCellReportV1> => {
  assertMatrixCellReportV1(report, plan);
  await assertMatrixCellReportDigest(report);
  return report;
};

export const validateIntegrationDecisionV1 = async (
  decision: IntegrationDecisionV1,
  plan?: MatrixPlanV1,
): Promise<IntegrationDecisionV1> => {
  assertIntegrationDecisionV1(decision, plan);
  await assertIntegrationDecisionDigest(decision);
  return decision;
};

export const validateMatrixCycleReportV1 = async (
  report: MatrixCycleReportV1,
  plan?: MatrixPlanV1,
): Promise<MatrixCycleReportV1> => {
  assertMatrixCycleReportV1(report, plan);
  await assertMatrixCycleReportDigest(report);
  return report;
};

/** Keep the import useful to downstream callers that start from a triage finding. */
export type MatrixFindingFromTriage =
  & Pick<TriageFinding, "id" | "fingerprint" | "validation_requirements">
  & Omit<MatrixFindingInput, "id" | "fingerprint" | "validation_requirements">;
