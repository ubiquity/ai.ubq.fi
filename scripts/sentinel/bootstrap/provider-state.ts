// Durable Provider Sentinel provider-state document schema (v1).
//
// This module is intentionally import-free: the provider state document is a
// self-contained contract shared by the fixed GitHub adapter. It records prior
// independently supplied attestation, promotion, and stop evidence; it never
// reads a provider health response and never proves inference itself.
//
// Every value is strictly validated and the parser constructs a NEW normalized
// object that is recursively frozen. Unknown keys, missing keys, invalid
// nulls, floats/overflow integers, malformed strings/times, duplicate
// application entries, and identity inconsistencies are all rejected with an
// error that carries only a safe field label — never the supplied value.

export const SENTINEL_PROVIDER_SCHEMA_VERSION = 1 as const;

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
/** DNS label: lowercase alphanumeric/hyphen, 1..63 chars, alphanumeric ends. */
const REVISION_ID = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/u;
/** Exactly canonical Date.toISOString() output: UTC, 3-digit milliseconds. */
const ISO_UTC = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/u;
/** Bounded nonempty reference: lowercase scheme followed by `:` and 1..480 safe chars, total at most 512. */
const BOUNDED_REFERENCE = /^[a-z][a-z0-9_-]*:[A-Za-z0-9._/-]{1,480}$/u;
/** Reason/transaction/invariant identifier. */
const IDENTIFIER = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/u;
/** Safe workflow basename under .github/workflows: lowercase alphanumeric/hyphen, no traversal. */
const WORKFLOW_PATH = /^\.github\/workflows\/[a-z0-9-]+\.yml$/u;
const REPOSITORY = "ubiquity/ai.ubq.fi" as const;
const APP_NAMES = ["ai-ubq-fi", "p-ai-ubq-fi"] as const;

const PHASES = [
  "prepared",
  "promotion_pending",
  "observing",
  "rollback_pending",
  "rollback_pending_verification",
  "kept",
  "rolled_back",
  "blocked",
] as const;

const DECISIONS = ["keep", "rollback", "dependency_failure", "ownership_unresolved"] as const;

const STOP_CONCLUSIONS = [
  "success",
  "failure",
  "cancelled",
  "timed_out",
  "action_required",
  "neutral",
  "skipped",
  "stale",
] as const;

const OBSERVATION_DEADLINE_MS = 30 * 60 * 1000;

export type SentinelProviderAppName = (typeof APP_NAMES)[number];

export type SentinelProviderAttestationV1 = Readonly<{
  git_sha: string;
  revision_id: string;
  configuration_digest: string;
  validator_sha: string;
  corpus_digest: string;
  verified_at: string;
  identity_ref: string;
  inference_ref: string;
}>;

export type SentinelProviderExecutorV1 = Readonly<{
  repository: string;
  workflow_path: string;
  run_id: number;
  run_attempt: number;
}>;

export type SentinelProviderStopConclusion = (typeof STOP_CONCLUSIONS)[number];

export type SentinelProviderStoppedExecutorV1 = Readonly<{
  executor: SentinelProviderExecutorV1;
  conclusion: SentinelProviderStopConclusion;
  observed_at: string;
  evidence_ref: string;
}>;

export type SentinelProviderPromotionResultV1 = Readonly<{
  kind: "acknowledged" | "ambiguous";
  http_status: number | null;
  observed_at: string;
  evidence_ref: string;
}>;

export type SentinelProviderObservationV1 = Readonly<{
  last_observed_at: string | null;
  samples: number;
  consecutive_liveness_failures: number;
  consecutive_inference_failures: number;
  invariant_id: string | null;
  consecutive_invariant_failures: number;
}>;

export type SentinelProviderRouteEvidenceV1 = Readonly<{
  revision_id: string;
  observed_at: string;
  evidence_ref: string;
}>;

export type SentinelProviderPhase = (typeof PHASES)[number];

export type SentinelProviderDecision = (typeof DECISIONS)[number];

export type SentinelProviderTransactionV1 = Readonly<{
  id: string;
  fence_generation: number;
  phase: SentinelProviderPhase;
  previous: SentinelProviderAttestationV1;
  candidate: SentinelProviderAttestationV1;
  expected_merged_sha: string;
  executor: SentinelProviderExecutorV1;
  retired_executor: SentinelProviderStoppedExecutorV1 | null;
  previous_transaction_commit: string | null;
  created_at: string;
  promotion_intent_at: string | null;
  promotion_result: SentinelProviderPromotionResultV1 | null;
  observation_deadline_at: string | null;
  observation: SentinelProviderObservationV1;
  route: SentinelProviderRouteEvidenceV1 | null;
  decision: SentinelProviderDecision | null;
  reason: string | null;
  rollback_intent_at: string | null;
  rollback_result: SentinelProviderPromotionResultV1 | null;
  restoration: SentinelProviderAttestationV1 | null;
}>;

export type SentinelProviderAppStateV1 = Readonly<{
  app: SentinelProviderAppName;
  healthy: SentinelProviderAttestationV1 | null;
  transaction: SentinelProviderTransactionV1 | null;
}>;

export type SentinelProviderStateDocumentV1 = Readonly<{
  schema_version: typeof SENTINEL_PROVIDER_SCHEMA_VERSION;
  generation: number;
  applications: readonly SentinelProviderAppStateV1[];
}>;

type JsonRecord = Record<string, unknown>;

const record = (value: unknown): JsonRecord | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as JsonRecord : null;

const mustObject = (value: unknown, label: string): JsonRecord => {
  const candidate = record(value);
  if (candidate === null) throw new Error(`${label} must be an object`);
  return candidate;
};

const onlyKeys = (value: JsonRecord, label: string, keys: readonly string[]): boolean => {
  for (const key of Object.keys(value)) {
    if (!keys.includes(key)) throw new Error(`${label} contains an unrecognized field`);
  }
  return true;
};

const field = (value: JsonRecord, key: string, label: string): unknown => {
  if (!Object.hasOwn(value, key)) throw new Error(`${label}.${key} is missing`);
  return value[key];
};

const fullSha = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !FULL_SHA.test(value)) {
    throw new Error(`${label} must be a lowercase 40-character Git SHA`);
  }
  return value;
};

const digest = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !SHA256.test(value)) {
    throw new Error(`${label} must be a lowercase 64-character digest`);
  }
  return value;
};

const revisionId = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !REVISION_ID.test(value)) {
    throw new Error(`${label} must be a lowercase DNS label of 1 to 63 characters`);
  }
  return value;
};

const positiveInteger = (value: unknown, label: string): number => {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${label} must be a positive safe integer`);
  }
  return value;
};

const nonNegativeInteger = (value: unknown, label: string): number => {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${label} must be a nonnegative safe integer`);
  }
  return value;
};

const isoTime = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !ISO_UTC.test(value)) throw new Error(`${label} must be an ISO UTC timestamp`);
  const parsed = Date.parse(value);
  if (!Number.isFinite(parsed) || new Date(parsed).toISOString() !== value) {
    throw new Error(`${label} must be a canonical UTC timestamp`);
  }
  return value;
};

const boundedReference = (value: unknown, label: string): string => {
  if (typeof value !== "string" || value.length > 512 || !BOUNDED_REFERENCE.test(value)) {
    throw new Error(`${label} must be a bounded nonempty reference`);
  }
  return value;
};

const identifier = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !IDENTIFIER.test(value)) {
    throw new Error(`${label} must be an identifier`);
  }
  return value;
};

const appName = (value: unknown, label: string): SentinelProviderAppName => {
  if (typeof value !== "string" || !(APP_NAMES as readonly string[]).includes(value)) {
    throw new Error(`${label} must be a known application`);
  }
  return value as SentinelProviderAppName;
};

const nullableTime = (value: unknown, label: string): string | null => value === null ? null : isoTime(value, label);

const nullableSha = (value: unknown, label: string): string | null => value === null ? null : fullSha(value, label);

const nullableIdentifier = (value: unknown, label: string): string | null =>
  value === null ? null : identifier(value, label);

const parseAttestation = (value: unknown, label: string): SentinelProviderAttestationV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, [
    "git_sha",
    "revision_id",
    "configuration_digest",
    "validator_sha",
    "corpus_digest",
    "verified_at",
    "identity_ref",
    "inference_ref",
  ]);
  return Object.freeze({
    git_sha: fullSha(field(parsed, "git_sha", label), `${label}.git_sha`),
    revision_id: revisionId(field(parsed, "revision_id", label), `${label}.revision_id`),
    configuration_digest: digest(field(parsed, "configuration_digest", label), `${label}.configuration_digest`),
    validator_sha: fullSha(field(parsed, "validator_sha", label), `${label}.validator_sha`),
    corpus_digest: digest(field(parsed, "corpus_digest", label), `${label}.corpus_digest`),
    verified_at: isoTime(field(parsed, "verified_at", label), `${label}.verified_at`),
    identity_ref: boundedReference(field(parsed, "identity_ref", label), `${label}.identity_ref`),
    inference_ref: boundedReference(field(parsed, "inference_ref", label), `${label}.inference_ref`),
  });
};

const parseExecutor = (value: unknown, label: string): SentinelProviderExecutorV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, ["repository", "workflow_path", "run_id", "run_attempt"]);
  const repository = field(parsed, "repository", label);
  if (repository !== REPOSITORY) throw new Error(`${label}.repository must be ${REPOSITORY}`);
  const workflowPath = field(parsed, "workflow_path", label);
  if (typeof workflowPath !== "string" || !WORKFLOW_PATH.test(workflowPath)) {
    throw new Error(`${label}.workflow_path must be a safe workflow path`);
  }
  return Object.freeze({
    repository: REPOSITORY,
    workflow_path: workflowPath,
    run_id: positiveInteger(field(parsed, "run_id", label), `${label}.run_id`),
    run_attempt: positiveInteger(field(parsed, "run_attempt", label), `${label}.run_attempt`),
  });
};

const parsePromotionResult = (value: unknown, label: string): SentinelProviderPromotionResultV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, ["kind", "http_status", "observed_at", "evidence_ref"]);
  const kind = field(parsed, "kind", label);
  const httpStatus = field(parsed, "http_status", label);
  if (kind !== "acknowledged" && kind !== "ambiguous") {
    throw new Error(`${label}.kind must be acknowledged or ambiguous`);
  }
  if (kind === "acknowledged" && httpStatus !== 204) {
    throw new Error(`${label} requires http_status 204 when acknowledged`);
  }
  if (kind === "ambiguous" && httpStatus !== null) {
    throw new Error(`${label} requires http_status null when ambiguous`);
  }
  return Object.freeze({
    kind,
    http_status: httpStatus as number | null,
    observed_at: isoTime(field(parsed, "observed_at", label), `${label}.observed_at`),
    evidence_ref: boundedReference(field(parsed, "evidence_ref", label), `${label}.evidence_ref`),
  });
};

const parseStoppedExecutor = (value: unknown, label: string): SentinelProviderStoppedExecutorV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, ["executor", "conclusion", "observed_at", "evidence_ref"]);
  const conclusion = field(parsed, "conclusion", label);
  if (typeof conclusion !== "string" || !(STOP_CONCLUSIONS as readonly string[]).includes(conclusion)) {
    throw new Error(`${label}.conclusion is invalid`);
  }
  return Object.freeze({
    executor: parseExecutor(field(parsed, "executor", label), `${label}.executor`),
    conclusion: conclusion as SentinelProviderStopConclusion,
    observed_at: isoTime(field(parsed, "observed_at", label), `${label}.observed_at`),
    evidence_ref: boundedReference(field(parsed, "evidence_ref", label), `${label}.evidence_ref`),
  });
};

const parseObservation = (value: unknown, label: string): SentinelProviderObservationV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, [
    "last_observed_at",
    "samples",
    "consecutive_liveness_failures",
    "consecutive_inference_failures",
    "invariant_id",
    "consecutive_invariant_failures",
  ]);
  const lastObservedAt = nullableTime(field(parsed, "last_observed_at", label), `${label}.last_observed_at`);
  const samples = nonNegativeInteger(field(parsed, "samples", label), `${label}.samples`);
  const livenessFailures = nonNegativeInteger(
    field(parsed, "consecutive_liveness_failures", label),
    `${label}.consecutive_liveness_failures`,
  );
  const inferenceFailures = nonNegativeInteger(
    field(parsed, "consecutive_inference_failures", label),
    `${label}.consecutive_inference_failures`,
  );
  const invariantId = nullableIdentifier(field(parsed, "invariant_id", label), `${label}.invariant_id`);
  const invariantFailures = nonNegativeInteger(
    field(parsed, "consecutive_invariant_failures", label),
    `${label}.consecutive_invariant_failures`,
  );
  if (samples === 0) {
    if (
      lastObservedAt !== null || livenessFailures !== 0 || inferenceFailures !== 0 ||
      invariantFailures !== 0 || invariantId !== null
    ) {
      throw new Error(`${label} must be empty when samples is 0`);
    }
  } else if (lastObservedAt === null) {
    throw new Error(`${label}.last_observed_at is required when samples is positive`);
  }
  if (
    livenessFailures > samples || inferenceFailures > samples || invariantFailures > samples
  ) {
    throw new Error(`${label} failure counters cannot exceed samples`);
  }
  if (invariantFailures > 0 && invariantId === null) {
    throw new Error(`${label}.invariant_id is required when invariant failures are recorded`);
  }
  return Object.freeze({
    last_observed_at: lastObservedAt,
    samples,
    consecutive_liveness_failures: livenessFailures,
    consecutive_inference_failures: inferenceFailures,
    invariant_id: invariantId,
    consecutive_invariant_failures: invariantFailures,
  });
};

const parseRouteEvidence = (value: unknown, label: string): SentinelProviderRouteEvidenceV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, ["revision_id", "observed_at", "evidence_ref"]);
  return Object.freeze({
    revision_id: revisionId(field(parsed, "revision_id", label), `${label}.revision_id`),
    observed_at: isoTime(field(parsed, "observed_at", label), `${label}.observed_at`),
    evidence_ref: boundedReference(field(parsed, "evidence_ref", label), `${label}.evidence_ref`),
  });
};

export const executorEquals = (left: SentinelProviderExecutorV1, right: SentinelProviderExecutorV1): boolean =>
  left.repository === right.repository && left.workflow_path === right.workflow_path &&
  left.run_id === right.run_id && left.run_attempt === right.run_attempt;

const attestationEquals = (
  left: SentinelProviderAttestationV1,
  right: SentinelProviderAttestationV1,
): boolean => JSON.stringify(left) === JSON.stringify(right);

const parseTransaction = (
  value: unknown,
  label: string,
  documentGeneration: number,
): SentinelProviderTransactionV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, [
    "id",
    "fence_generation",
    "phase",
    "previous",
    "candidate",
    "expected_merged_sha",
    "executor",
    "retired_executor",
    "previous_transaction_commit",
    "created_at",
    "promotion_intent_at",
    "promotion_result",
    "observation_deadline_at",
    "observation",
    "route",
    "decision",
    "reason",
    "rollback_intent_at",
    "rollback_result",
    "restoration",
  ]);
  const id = identifier(field(parsed, "id", label), `${label}.id`);
  const fenceGeneration = positiveInteger(field(parsed, "fence_generation", label), `${label}.fence_generation`);
  if (fenceGeneration > documentGeneration) {
    throw new Error(`${label}.fence_generation cannot exceed the document generation`);
  }
  const phase = field(parsed, "phase", label);
  if (typeof phase !== "string" || !(PHASES as readonly string[]).includes(phase)) {
    throw new Error(`${label}.phase is invalid`);
  }
  const previous = parseAttestation(field(parsed, "previous", label), `${label}.previous`);
  const candidate = parseAttestation(field(parsed, "candidate", label), `${label}.candidate`);
  const expectedMergedSha = fullSha(field(parsed, "expected_merged_sha", label), `${label}.expected_merged_sha`);
  const executor = parseExecutor(field(parsed, "executor", label), `${label}.executor`);
  const retiredExecutorRaw = field(parsed, "retired_executor", label);
  const retiredExecutor = retiredExecutorRaw === null
    ? null
    : parseStoppedExecutor(retiredExecutorRaw, `${label}.retired_executor`);
  const previousTransactionCommit = nullableSha(
    field(parsed, "previous_transaction_commit", label),
    `${label}.previous_transaction_commit`,
  );
  const createdAt = isoTime(field(parsed, "created_at", label), `${label}.created_at`);
  const promotionIntentAt = nullableTime(
    field(parsed, "promotion_intent_at", label),
    `${label}.promotion_intent_at`,
  );
  const promotionResultRaw = field(parsed, "promotion_result", label);
  const promotionResult = promotionResultRaw === null
    ? null
    : parsePromotionResult(promotionResultRaw, `${label}.promotion_result`);
  const observationDeadlineAt = nullableTime(
    field(parsed, "observation_deadline_at", label),
    `${label}.observation_deadline_at`,
  );
  const observation = parseObservation(field(parsed, "observation", label), `${label}.observation`);
  const routeRaw = field(parsed, "route", label);
  const route = routeRaw === null ? null : parseRouteEvidence(routeRaw, `${label}.route`);
  const decisionRaw = field(parsed, "decision", label);
  const decision = decisionRaw === null
    ? null
    : typeof decisionRaw === "string" && (DECISIONS as readonly string[]).includes(decisionRaw)
    ? decisionRaw as SentinelProviderDecision
    : null;
  if (decisionRaw !== null && decision === null) throw new Error(`${label}.decision is invalid`);
  const reason = nullableIdentifier(field(parsed, "reason", label), `${label}.reason`);
  const rollbackIntentAt = nullableTime(
    field(parsed, "rollback_intent_at", label),
    `${label}.rollback_intent_at`,
  );
  const rollbackResultRaw = field(parsed, "rollback_result", label);
  const rollbackResult = rollbackResultRaw === null
    ? null
    : parsePromotionResult(rollbackResultRaw, `${label}.rollback_result`);
  const restorationRaw = field(parsed, "restoration", label);
  const restoration = restorationRaw === null ? null : parseAttestation(restorationRaw, `${label}.restoration`);

  // Identity consistency: previous/candidate differ, candidate pinned by the
  // expected merged SHA, and the durable config/validator/corpus match.
  if (previous.git_sha === candidate.git_sha || previous.revision_id === candidate.revision_id) {
    throw new Error(`${label} previous and candidate must differ in both SHA and revision`);
  }
  if (expectedMergedSha !== candidate.git_sha) {
    throw new Error(`${label}.expected_merged_sha must equal the candidate Git SHA`);
  }
  if (
    previous.configuration_digest !== candidate.configuration_digest ||
    previous.validator_sha !== candidate.validator_sha ||
    previous.corpus_digest !== candidate.corpus_digest
  ) {
    throw new Error(`${label} candidate config/validator/corpus must match previous`);
  }
  if (previous.verified_at > createdAt || candidate.verified_at > createdAt) {
    throw new Error(`${label} attestations cannot be verified after creation`);
  }
  if (retiredExecutor !== null) {
    if (executorEquals(retiredExecutor.executor, executor)) {
      throw new Error(`${label}.retired_executor must differ from the current executor`);
    }
    if (retiredExecutor.observed_at < createdAt) {
      throw new Error(`${label}.retired_executor.observed_at must be at or after created_at`);
    }
  }
  if (promotionResult !== null) {
    if (promotionIntentAt === null) {
      throw new Error(`${label}.promotion_result requires promotion_intent_at`);
    }
    if (promotionResult.observed_at < promotionIntentAt) {
      throw new Error(`${label}.promotion_result cannot precede promotion_intent_at`);
    }
  }
  if (rollbackIntentAt !== null && promotionIntentAt === null) {
    throw new Error(`${label}.rollback_intent_at requires promotion_intent_at`);
  }
  if (rollbackIntentAt !== null && promotionIntentAt !== null && rollbackIntentAt < promotionIntentAt) {
    throw new Error(`${label}.rollback_intent_at cannot precede promotion_intent_at`);
  }
  if (rollbackResult !== null) {
    if (rollbackIntentAt === null) {
      throw new Error(`${label}.rollback_result requires rollback_intent_at`);
    }
    if (rollbackResult.observed_at < rollbackIntentAt) {
      throw new Error(`${label}.rollback_result cannot precede rollback_intent_at`);
    }
  }
  if (restoration !== null) {
    if (rollbackIntentAt === null) {
      throw new Error(`${label}.restoration requires rollback_intent_at`);
    }
    if (restoration.verified_at < rollbackIntentAt) {
      throw new Error(`${label}.restoration cannot be verified before rollback_intent_at`);
    }
    if (
      restoration.git_sha !== previous.git_sha ||
      restoration.revision_id !== previous.revision_id ||
      restoration.configuration_digest !== previous.configuration_digest ||
      restoration.validator_sha !== previous.validator_sha ||
      restoration.corpus_digest !== previous.corpus_digest
    ) {
      throw new Error(`${label}.restoration must match the previous identity`);
    }
  }
  if (
    observation.last_observed_at !== null && promotionIntentAt !== null &&
    observation.last_observed_at < promotionIntentAt
  ) {
    throw new Error(`${label}.observation cannot precede promotion_intent_at`);
  }
  if (
    route !== null && route.revision_id !== candidate.revision_id &&
    route.revision_id !== previous.revision_id
  ) {
    throw new Error(`${label}.route must identify the candidate or previous revision`);
  }
  if (decision !== null && reason === null) {
    throw new Error(`${label}.reason is required for a decision`);
  }

  if (phase === "prepared") {
    if (promotionIntentAt !== null || promotionResult !== null || observationDeadlineAt !== null) {
      throw new Error(`${label} prepared phase must not carry promotion intent, result, or deadline`);
    }
    if (route !== null || observation.samples !== 0) {
      throw new Error(`${label} prepared phase must not carry a route or observation samples`);
    }
    if (decision !== null || reason !== null) {
      throw new Error(`${label} prepared phase must not carry a decision or reason`);
    }
    if (rollbackIntentAt !== null || rollbackResult !== null || restoration !== null) {
      throw new Error(`${label} prepared phase must not carry rollback or restoration`);
    }
  } else {
    if (promotionIntentAt === null || promotionIntentAt < createdAt) {
      throw new Error(`${label} requires promotion_intent_at at or after created_at`);
    }
    if (
      observationDeadlineAt === null ||
      Date.parse(observationDeadlineAt) - Date.parse(promotionIntentAt) !== OBSERVATION_DEADLINE_MS
    ) {
      throw new Error(`${label} observation_deadline_at must be exactly 30 minutes after promotion_intent_at`);
    }
    if (phase === "promotion_pending") {
      if (promotionResult !== null && promotionResult.kind !== "ambiguous") {
        throw new Error(`${label} promotion_pending only permits a null or ambiguous promotion_result`);
      }
      if (decision !== null || reason !== null) {
        throw new Error(`${label} promotion_pending must not carry a decision or reason`);
      }
      if (rollbackIntentAt !== null || rollbackResult !== null || restoration !== null) {
        throw new Error(`${label} promotion_pending must not carry rollback or restoration`);
      }
    } else if (phase === "observing") {
      if (promotionResult === null) {
        throw new Error(`${label} observing requires a promotion_result`);
      }
      if (decision !== null || reason !== null) {
        throw new Error(`${label} observing must not carry a decision or reason`);
      }
      if (rollbackIntentAt !== null || rollbackResult !== null || restoration !== null) {
        throw new Error(`${label} observing must not carry rollback or restoration`);
      }
    } else if (phase === "rollback_pending") {
      if (decision !== "rollback" || reason === null || rollbackIntentAt === null) {
        throw new Error(`${label} rollback_pending requires rollback decision, reason, and intent`);
      }
      if (rollbackResult !== null && rollbackResult.kind !== "ambiguous") {
        throw new Error(`${label} rollback_pending only permits a null or ambiguous rollback_result`);
      }
      if (restoration !== null) throw new Error(`${label} rollback_pending must not carry restoration`);
    } else if (phase === "rollback_pending_verification") {
      if (decision !== "rollback" || reason === null || rollbackIntentAt === null) {
        throw new Error(`${label} rollback_pending_verification requires rollback decision, reason, and intent`);
      }
      if (rollbackResult === null || rollbackResult.kind !== "acknowledged") {
        throw new Error(`${label} rollback_pending_verification requires an acknowledged rollback_result`);
      }
      if (restoration !== null) throw new Error(`${label} rollback_pending_verification must not carry restoration`);
    } else if (phase === "kept") {
      if (decision !== "keep" || reason === null) {
        throw new Error(`${label} kept requires the keep decision and a reason`);
      }
      if (promotionResult === null || promotionResult.kind !== "acknowledged") {
        throw new Error(`${label} kept requires an acknowledged promotion_result`);
      }
      if (
        observation.last_observed_at === null ||
        observation.last_observed_at < observationDeadlineAt
      ) {
        throw new Error(`${label} kept requires an observation at or after the deadline`);
      }
      if (
        observation.consecutive_liveness_failures !== 0 ||
        observation.consecutive_inference_failures !== 0 ||
        observation.consecutive_invariant_failures !== 0
      ) {
        throw new Error(`${label} kept requires zero consecutive failures`);
      }
      if (route === null || route.revision_id !== candidate.revision_id) {
        throw new Error(`${label} kept requires the exact candidate route`);
      }
      if (route.observed_at < observation.last_observed_at) {
        throw new Error(`${label}.route.observed_at cannot precede the kept observation`);
      }
      if (promotionResult.observed_at > observation.last_observed_at) {
        throw new Error(`${label}.promotion_result.observed_at cannot follow the kept observation`);
      }
      if (rollbackIntentAt !== null || rollbackResult !== null || restoration !== null) {
        throw new Error(`${label} kept must not carry rollback or restoration`);
      }
    } else if (phase === "rolled_back") {
      if (decision !== "rollback" || reason === null || rollbackIntentAt === null) {
        throw new Error(`${label} rolled_back requires rollback decision, reason, and intent`);
      }
      if (rollbackResult === null || rollbackResult.kind !== "acknowledged") {
        throw new Error(`${label} rolled_back requires an acknowledged rollback_result`);
      }
      if (route === null || route.revision_id !== previous.revision_id) {
        throw new Error(`${label} rolled_back requires the exact previous route`);
      }
      if (route.observed_at < rollbackResult.observed_at) {
        throw new Error(`${label}.route.observed_at cannot precede the rollback result`);
      }
      if (restoration === null) throw new Error(`${label} rolled_back requires restoration`);
      if (restoration.verified_at < rollbackResult.observed_at) {
        throw new Error(`${label}.restoration.verified_at cannot precede the rollback result`);
      }
    } else if (phase === "blocked") {
      if (decision !== "dependency_failure" && decision !== "ownership_unresolved") {
        throw new Error(`${label} blocked requires a dependency_failure or ownership_unresolved decision`);
      }
      if (reason === null) throw new Error(`${label} blocked requires a reason`);
      if (restoration !== null) throw new Error(`${label} blocked must not carry restoration`);
    }
  }

  const transaction: SentinelProviderTransactionV1 = Object.freeze({
    id,
    fence_generation: fenceGeneration,
    phase: phase as SentinelProviderPhase,
    previous,
    candidate,
    expected_merged_sha: expectedMergedSha,
    executor,
    retired_executor: retiredExecutor,
    previous_transaction_commit: previousTransactionCommit,
    created_at: createdAt,
    promotion_intent_at: promotionIntentAt,
    promotion_result: promotionResult,
    observation_deadline_at: observationDeadlineAt,
    observation,
    route,
    decision,
    reason,
    rollback_intent_at: rollbackIntentAt,
    rollback_result: rollbackResult,
    restoration,
  });
  return transaction;
};

const parseAppState = (value: unknown, label: string, documentGeneration: number): SentinelProviderAppStateV1 => {
  const parsed = mustObject(value, label);
  onlyKeys(parsed, label, ["app", "healthy", "transaction"]);
  const app = appName(field(parsed, "app", label), `${label}.app`);
  const healthyRaw = field(parsed, "healthy", label);
  const healthy = healthyRaw === null ? null : parseAttestation(healthyRaw, `${label}.healthy`);
  const transactionRaw = field(parsed, "transaction", label);
  const transaction = transactionRaw === null
    ? null
    : parseTransaction(transactionRaw, `${label}.transaction`, documentGeneration);
  if (healthy === null && transaction !== null) {
    throw new Error(`${label}.transaction requires an initialized healthy attestation`);
  }
  if (transaction !== null) {
    const matches = (attestation: SentinelProviderAttestationV1): boolean =>
      healthy !== null && attestationEquals(healthy, attestation);
    if (transaction.phase === "kept" && !matches(transaction.candidate)) {
      throw new Error(`${label}.healthy must exactly equal the kept candidate`);
    } else if (transaction.phase === "rolled_back" && !matches(transaction.restoration!)) {
      throw new Error(`${label}.healthy must exactly equal the restoration`);
    } else if (transaction.phase !== "kept" && transaction.phase !== "rolled_back" && !matches(transaction.previous)) {
      throw new Error(`${label}.healthy must exactly equal the previous attestation`);
    }
  }
  return Object.freeze({ app, healthy, transaction });
};

export const parseSentinelProviderStateDocument = (value: unknown): SentinelProviderStateDocumentV1 => {
  const parsed = mustObject(value, "provider_state");
  onlyKeys(parsed, "provider_state", ["schema_version", "generation", "applications"]);
  if (field(parsed, "schema_version", "provider_state") !== SENTINEL_PROVIDER_SCHEMA_VERSION) {
    throw new Error("provider_state.schema_version must be 1");
  }
  const generation = positiveInteger(field(parsed, "generation", "provider_state"), "provider_state.generation");
  const applicationsRaw = field(parsed, "applications", "provider_state");
  if (!Array.isArray(applicationsRaw) || applicationsRaw.length < 1 || applicationsRaw.length > 2) {
    throw new Error("provider_state.applications must contain 1 to 2 entries");
  }
  const applications = applicationsRaw.map((entry, index) =>
    parseAppState(entry, `provider_state.applications[${index}]`, generation)
  );
  const appNames = applications.map((entry) => entry.app);
  if (new Set(appNames).size !== appNames.length) {
    throw new Error("provider_state.applications contain duplicate apps");
  }
  applications.sort((left, right) => (left.app < right.app ? -1 : left.app > right.app ? 1 : 0));
  return Object.freeze({
    schema_version: SENTINEL_PROVIDER_SCHEMA_VERSION,
    generation,
    applications: Object.freeze(applications),
  });
};

export const isTerminalSentinelProviderPhase = (phase: SentinelProviderPhase): boolean =>
  phase === "kept" || phase === "rolled_back";
