export const SENTINEL_BOOTSTRAP_SCHEMA_VERSION = 1 as const;
export const SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION = 1 as const;

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
/** Deterministic non-cryptographic advisory digest (FNV-1a 64-bit hex). */
const ADVISORY_DIGEST = /^[0-9a-f]{16}$/u;
const ISO_TIMESTAMP = /^\d{4}-\d{2}-\d{2}T/u;
const OPAQUE_REFERENCE = /^[A-Za-z0-9][A-Za-z0-9._:/-]{0,255}$/u;

export type SentinelBootstrapFailureClass =
  | "invalid_startup_policy"
  | "workflow_failure"
  | "checkpoint_failure"
  | "corrupted_state_transition"
  | "canary_acceptance_failure"
  | "provider_timeout"
  | "network_error"
  | "provider_5xx"
  | "luna_capacity_failure";

export const AUTHORITATIVE_BOOTSTRAP_FAILURE_CLASSES: readonly SentinelBootstrapFailureClass[] = Object.freeze([
  "invalid_startup_policy",
  "workflow_failure",
  "checkpoint_failure",
  "corrupted_state_transition",
  "canary_acceptance_failure",
]);

export const TRANSIENT_BOOTSTRAP_FAILURE_CLASSES: readonly SentinelBootstrapFailureClass[] = Object.freeze([
  "provider_timeout",
  "network_error",
  "provider_5xx",
  "luna_capacity_failure",
]);

export type SentinelBootstrapActivationPointerV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_SCHEMA_VERSION;
  active_sha: string;
  generation: number;
  fenced_generations: readonly number[];
  updated_at: string;
  reason: string | null;
}>;

export type SentinelBootstrapHealthSignalV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_SCHEMA_VERSION;
  generation: number;
  failure_class: SentinelBootstrapFailureClass;
  failure_fingerprint: string;
  observed_at: string;
  evidence_refs: readonly string[];
  /** Optional event identity used to avoid counting a duplicated observation twice. */
  observation_id?: string;
}>;

export type SentinelFailureConstraintV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_SCHEMA_VERSION;
  fenced_generation: number;
  failure_class:
    | "invalid_startup_policy"
    | "workflow_failure"
    | "checkpoint_failure"
    | "corrupted_state_transition"
    | "canary_acceptance_failure";
  failure_fingerprint: string;
  violated_invariant: string;
  evidence_refs: readonly string[];
  regression_test: string;
  created_at: string;
}>;

export type SentinelBootstrapRollbackIntentV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_SCHEMA_VERSION;
  previous_sha: string;
  target_sha: string;
  fenced_generation: number;
  active_generation: number;
  constraint: SentinelFailureConstraintV1;
  created_at: string;
}>;

export type SentinelBootstrapReleaseRecordV1 = Readonly<{
  schema_version: 1;
  stable_sha: string;
  candidate_sha: string | null;
  acceptance_evidence: readonly string[];
  activated_at: string;
  rollback_reason: string | null;
  generation: number;
}>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const positiveInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const nonEmpty = (value: unknown): value is string => typeof value === "string" && value.trim().length > 0;

const timestamp = (value: unknown): value is string =>
  nonEmpty(value) && ISO_TIMESTAMP.test(value) && Number.isFinite(Date.parse(value));

const boundedReference = (value: unknown): value is string => typeof value === "string" && OPAQUE_REFERENCE.test(value);

const boundedReferences = (value: unknown): value is readonly string[] =>
  Array.isArray(value) && value.length <= 8 && value.every(boundedReference);

const requiredReferences = (value: unknown): value is readonly string[] => boundedReferences(value) && value.length > 0;

const validFenceHistory = (value: unknown, generation: unknown): value is readonly number[] =>
  typeof generation === "number" &&
  Array.isArray(value) && value.length <= 64 &&
  value.every(positiveInteger) &&
  new Set(value).size === value.length &&
  value.every((fencedGeneration) => fencedGeneration < generation);

const failureClass = (value: unknown): value is SentinelBootstrapFailureClass =>
  typeof value === "string" &&
  (AUTHORITATIVE_BOOTSTRAP_FAILURE_CLASSES.includes(value as SentinelBootstrapFailureClass) ||
    TRANSIENT_BOOTSTRAP_FAILURE_CLASSES.includes(value as SentinelBootstrapFailureClass));

export const isAuthoritativeBootstrapFailureClass = (
  value: unknown,
): value is SentinelFailureConstraintV1["failure_class"] =>
  typeof value === "string" && AUTHORITATIVE_BOOTSTRAP_FAILURE_CLASSES.includes(value as SentinelBootstrapFailureClass);

export const isTransientBootstrapFailureClass = (value: unknown): value is SentinelBootstrapFailureClass =>
  typeof value === "string" && TRANSIENT_BOOTSTRAP_FAILURE_CLASSES.includes(value as SentinelBootstrapFailureClass);

export const parseSentinelBootstrapActivationPointer = (
  value: unknown,
): SentinelBootstrapActivationPointerV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_SCHEMA_VERSION ||
    typeof value.active_sha !== "string" || !FULL_SHA.test(value.active_sha) || !positiveInteger(value.generation) ||
    !validFenceHistory(value.fenced_generations, value.generation) || !timestamp(value.updated_at) ||
    !(value.reason === null || boundedReference(value.reason))
  ) throw new Error("Sentinel bootstrap activation pointer is invalid");
  return value as SentinelBootstrapActivationPointerV1;
};

export const parseSentinelBootstrapHealthSignal = (
  value: unknown,
): SentinelBootstrapHealthSignalV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_SCHEMA_VERSION ||
    !positiveInteger(value.generation) ||
    !failureClass(value.failure_class) || typeof value.failure_fingerprint !== "string" ||
    !SHA256.test(value.failure_fingerprint) || !timestamp(value.observed_at) ||
    !requiredReferences(value.evidence_refs) ||
    (value.observation_id !== undefined && !boundedReference(value.observation_id))
  ) throw new Error("Sentinel bootstrap health signal is invalid");
  return value as SentinelBootstrapHealthSignalV1;
};

export const parseSentinelFailureConstraint = (value: unknown): SentinelFailureConstraintV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_SCHEMA_VERSION ||
    !positiveInteger(value.fenced_generation) || !isAuthoritativeBootstrapFailureClass(value.failure_class) ||
    typeof value.failure_fingerprint !== "string" || !SHA256.test(value.failure_fingerprint) ||
    !boundedReference(value.violated_invariant) || !requiredReferences(value.evidence_refs) ||
    !boundedReference(value.regression_test) || !timestamp(value.created_at)
  ) throw new Error("Sentinel failure constraint is invalid");
  return value as SentinelFailureConstraintV1;
};

export const parseSentinelBootstrapRollbackIntent = (value: unknown): SentinelBootstrapRollbackIntentV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_SCHEMA_VERSION ||
    typeof value.previous_sha !== "string" || !FULL_SHA.test(value.previous_sha) ||
    typeof value.target_sha !== "string" || !FULL_SHA.test(value.target_sha) ||
    value.previous_sha === value.target_sha || !positiveInteger(value.fenced_generation) ||
    !positiveInteger(value.active_generation) || value.active_generation !== value.fenced_generation + 1 ||
    !timestamp(value.created_at)
  ) throw new Error("Sentinel bootstrap rollback intent is invalid");
  parseSentinelFailureConstraint(value.constraint);
  return value as SentinelBootstrapRollbackIntentV1;
};

export const parseBootstrapReleaseRecord = (value: unknown): SentinelBootstrapReleaseRecordV1 => {
  if (
    !isRecord(value) || value.schema_version !== 1 || typeof value.stable_sha !== "string" ||
    !FULL_SHA.test(value.stable_sha) ||
    !(value.candidate_sha === null ||
      (typeof value.candidate_sha === "string" && FULL_SHA.test(value.candidate_sha))) ||
    !Array.isArray(value.acceptance_evidence) || !value.acceptance_evidence.every(boundedReference) ||
    !timestamp(value.activated_at) ||
    !(value.rollback_reason === null || boundedReference(value.rollback_reason)) || !positiveInteger(value.generation)
  ) throw new Error("Sentinel bootstrap release record is invalid");
  if (value.acceptance_evidence.length === 0) {
    throw new Error("Sentinel bootstrap release record has no acceptance evidence");
  }
  return value as SentinelBootstrapReleaseRecordV1;
};

export type SentinelBootstrapProgressVerdict = "progress" | "stuck" | "ambiguous";
export type SentinelBootstrapProgressResolvedVerdict = "progress" | "stuck" | "unknown";

/**
 * One canonical per-run progress observation (plan m06). Every field is
 * required: a missing key makes the observation invalid and fails closed.
 * `run_id` and `source` identify the observation; they are deliberately NOT
 * part of the canonical durable state used for cycle detection, because a new
 * run of an identical dead state is exactly a stuck loop.
 */
export type SentinelBootstrapProgressObservationV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION;
  run_id: string;
  source: string;
  generation: number | null;
  phase: string | null;
  milestone: string | null;
  failure_fingerprint: string | null;
  git_sha: string | null;
  ledger_version: number | null;
  retry_state: string | null;
  verification_evidence: string | null;
}>;

/**
 * Advisory evidence from the one zero-tool GPT-OSS classifier call. It is
 * never authoritative: it cannot override health/rollback identity, the exact
 * Git SHA or immutable revision proof, and it cannot authorize promotion.
 */
export type SentinelBootstrapClassifierEvidenceV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION;
  answer: "true" | "false" | "unknown";
  raw: string | null;
  /** Bounded snake_case reason token (no surrounding prose). */
  reason: string;
  requested_model: string;
  status: number | null;
  requested_at: string;
  observation_digest: string;
  advisory: true;
}>;

/**
 * A bootstrap progress decision. `verdict` is the pure deterministic outcome
 * computed before any model call; `resolved` is the fail-closed final decision
 * after the optional classifier call. This evidence is advisory in the current
 * bootstrap and is not consumed by promotion. Any later authority change must
 * retain the exact immutable revision and full Git SHA health proof.
 */
export type SentinelBootstrapProgressDecisionV1 = Readonly<{
  schema_version: typeof SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION;
  verdict: SentinelBootstrapProgressVerdict;
  reason: string;
  observation_count: number;
  state_digest: string;
  classifier: SentinelBootstrapClassifierEvidenceV1 | null;
  resolved: SentinelBootstrapProgressResolvedVerdict;
  resolved_reason: string;
  evaluated_at: string;
}>;

const progressVerdict = (value: unknown): value is SentinelBootstrapProgressVerdict =>
  value === "progress" || value === "stuck" || value === "ambiguous";

const resolvedProgressVerdict = (value: unknown): value is SentinelBootstrapProgressResolvedVerdict =>
  value === "progress" || value === "stuck" || value === "unknown";

const nullablePositiveInteger = (value: unknown): value is number | null => value === null || positiveInteger(value);

const nullableReference = (value: unknown): value is string | null => value === null || boundedReference(value);

const nullableFullSha = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && FULL_SHA.test(value));

const nullableSha256 = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && SHA256.test(value));

const httpStatus = (value: unknown): value is number | null =>
  value === null ||
  (typeof value === "number" && Number.isInteger(value) && value >= 0 && value <= 599);

const classifierAnswer = (value: unknown): value is "true" | "false" | "unknown" =>
  value === "true" || value === "false" || value === "unknown";

const advisoryDigest = (value: unknown): value is string => typeof value === "string" && ADVISORY_DIGEST.test(value);

export const parseSentinelBootstrapProgressObservation = (
  value: unknown,
): SentinelBootstrapProgressObservationV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION ||
    !boundedReference(value.run_id) || !boundedReference(value.source) ||
    !nullablePositiveInteger(value.generation) || !nullableReference(value.phase) ||
    !nullableReference(value.milestone) || !nullableSha256(value.failure_fingerprint) ||
    !nullableFullSha(value.git_sha) || !nullablePositiveInteger(value.ledger_version) ||
    !nullableReference(value.retry_state) || !nullableReference(value.verification_evidence)
  ) throw new Error("Sentinel bootstrap progress observation is invalid");
  return value as SentinelBootstrapProgressObservationV1;
};

export const parseSentinelBootstrapClassifierEvidence = (
  value: unknown,
): SentinelBootstrapClassifierEvidenceV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION ||
    !classifierAnswer(value.answer) ||
    !(value.raw === null || (typeof value.raw === "string" && value.raw.length <= 512)) ||
    !boundedReference(value.reason) || !boundedReference(value.requested_model) ||
    !httpStatus(value.status) || !timestamp(value.requested_at) ||
    !advisoryDigest(value.observation_digest) || value.advisory !== true
  ) throw new Error("Sentinel bootstrap classifier evidence is invalid");
  return value as SentinelBootstrapClassifierEvidenceV1;
};

export const parseSentinelBootstrapProgressDecision = (
  value: unknown,
): SentinelBootstrapProgressDecisionV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_BOOTSTRAP_PROGRESS_SCHEMA_VERSION ||
    !progressVerdict(value.verdict) || !boundedReference(value.reason) ||
    !positiveInteger(value.observation_count) || !advisoryDigest(value.state_digest) ||
    !(value.classifier === null || parseSentinelBootstrapClassifierEvidence(value.classifier)) ||
    !resolvedProgressVerdict(value.resolved) || !boundedReference(value.resolved_reason) ||
    !timestamp(value.evaluated_at)
  ) throw new Error("Sentinel bootstrap progress decision is invalid");
  return value as SentinelBootstrapProgressDecisionV1;
};

export const assertFullGitSha = (value: string, label: string): string => {
  if (!FULL_SHA.test(value)) throw new Error(`${label} must be a lowercase full Git SHA`);
  return value;
};

export const assertPositiveGeneration = (value: number, label: string): number => {
  if (!positiveInteger(value)) throw new Error(`${label} must be a positive integer`);
  return value;
};

export const assertBootstrapTimestamp = (value: string, label: string): string => {
  if (!timestamp(value)) throw new Error(`${label} must be an ISO timestamp`);
  return value;
};

export const assertBootstrapReference = (value: string, label: string): string => {
  if (!boundedReference(value)) throw new Error(`${label} is invalid`);
  return value;
};
