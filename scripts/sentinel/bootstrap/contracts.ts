import { parseSentinelReleaseRecord, type SentinelReleaseRecordV1 } from "../recovery.ts";
import { isRecord } from "../../../src/utils.ts";

export const SENTINEL_BOOTSTRAP_SCHEMA_VERSION = 1 as const;

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
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

export const parseBootstrapReleaseRecord = (value: unknown): SentinelReleaseRecordV1 => {
  const release = parseSentinelReleaseRecord(value);
  if (release.acceptance_evidence.length === 0) {
    throw new Error("Sentinel bootstrap release record has no acceptance evidence");
  }
  return release;
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
