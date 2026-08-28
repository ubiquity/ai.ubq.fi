import { isRecord } from "../../src/utils.ts";

const FULL_SHA = /^[0-9a-f]{40}$/u;
const SHA256 = /^[0-9a-f]{64}$/u;
const ISO_TIMESTAMP = /^\d{4}-\d{2}-\d{2}T/u;

export const SENTINEL_RECOVERY_SCHEMA_VERSION = 1 as const;

export type SentinelRecoveryPhase =
  | "claimed"
  | "implementation_running"
  | "workspace_dirty"
  | "checkpoint_publishing"
  | "checkpoint_durable"
  | "validation_failed"
  | "review_pending"
  | "retry_wait"
  | "recovery_pending"
  | "manual_required"
  | "rejected"
  | "delivered";

export type SentinelRecoveryDisposition = "active" | "manual_required" | "rejected" | "delivered";
export type SentinelRecoverySourceKind = "github_issue" | "review_backlog" | "triage" | "incident";

export type SentinelRecoveryIdentityV1 = Readonly<{
  repository: string;
  source_kind: SentinelRecoverySourceKind;
  source_id: string;
  source_revision: string;
  candidate_generation: number;
}>;

export type SentinelRecoveryRecordV1 = Readonly<{
  schema_version: typeof SENTINEL_RECOVERY_SCHEMA_VERSION;
  identity: SentinelRecoveryIdentityV1;
  run_id: string;
  attempt: number;
  lease_token: string;
  base_sha: string;
  phase: SentinelRecoveryPhase;
  disposition: SentinelRecoveryDisposition;
  state_version: number;
  created_at: string;
  updated_at: string;
  candidate_branch: string | null;
  candidate_sha: string | null;
  changed_files: readonly string[];
  tree_sha: string | null;
  failure_class: string | null;
  failure_fingerprint: string | null;
  artifact_ids: readonly number[];
  artifact_digests: readonly string[];
  reason: string | null;
  next_action: string | null;
  predecessor: string | null;
}>;

export type SentinelReleaseRecordV1 = Readonly<{
  schema_version: 1;
  stable_sha: string;
  candidate_sha: string | null;
  acceptance_evidence: readonly string[];
  activated_at: string;
  rollback_reason: string | null;
  generation: number;
}>;

const phases = new Set<SentinelRecoveryPhase>([
  "claimed",
  "implementation_running",
  "workspace_dirty",
  "checkpoint_publishing",
  "checkpoint_durable",
  "validation_failed",
  "review_pending",
  "retry_wait",
  "recovery_pending",
  "manual_required",
  "rejected",
  "delivered",
]);

const terminalPhases = new Set<SentinelRecoveryPhase>(["manual_required", "rejected", "delivered"]);
const transitions: Readonly<Record<SentinelRecoveryPhase, readonly SentinelRecoveryPhase[]>> = {
  claimed: ["implementation_running", "recovery_pending", "rejected", "manual_required"],
  implementation_running: [
    "workspace_dirty",
    "checkpoint_durable",
    "retry_wait",
    "recovery_pending",
    "rejected",
    "manual_required",
  ],
  workspace_dirty: ["checkpoint_publishing", "recovery_pending", "manual_required"],
  checkpoint_publishing: ["checkpoint_durable", "recovery_pending", "manual_required"],
  checkpoint_durable: ["validation_failed", "review_pending", "retry_wait", "recovery_pending", "manual_required"],
  validation_failed: ["retry_wait", "manual_required", "rejected"],
  review_pending: ["retry_wait", "manual_required", "rejected", "delivered"],
  retry_wait: ["claimed", "recovery_pending", "manual_required"],
  recovery_pending: ["checkpoint_publishing", "checkpoint_durable", "retry_wait", "manual_required", "rejected"],
  manual_required: [],
  rejected: [],
  delivered: [],
};

const nonEmpty = (value: unknown): value is string => typeof value === "string" && value.trim().length > 0;
const isoTimestamp = (value: unknown): value is string =>
  nonEmpty(value) && ISO_TIMESTAMP.test(value) && Number.isFinite(Date.parse(value));
const positiveInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;
const optionalSha = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && FULL_SHA.test(value));

const isIdentity = (value: unknown): value is SentinelRecoveryIdentityV1 =>
  isRecord(value) && nonEmpty(value.repository) &&
  (value.source_kind === "github_issue" || value.source_kind === "review_backlog" || value.source_kind === "triage" ||
    value.source_kind === "incident") &&
  nonEmpty(value.source_id) && nonEmpty(value.source_revision) && positiveInteger(value.candidate_generation);

const sameIdentity = (left: SentinelRecoveryIdentityV1, right: SentinelRecoveryIdentityV1): boolean =>
  left.repository === right.repository && left.source_kind === right.source_kind &&
  left.source_id === right.source_id &&
  left.source_revision === right.source_revision && left.candidate_generation === right.candidate_generation;

export const isTerminalRecoveryPhase = (phase: SentinelRecoveryPhase): boolean => terminalPhases.has(phase);

export const parseSentinelRecoveryRecord = (value: unknown): SentinelRecoveryRecordV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_RECOVERY_SCHEMA_VERSION || !isIdentity(value.identity) ||
    !nonEmpty(value.run_id) || !positiveInteger(value.attempt) || !nonEmpty(value.lease_token) ||
    typeof value.base_sha !== "string" || !FULL_SHA.test(value.base_sha) || typeof value.phase !== "string" ||
    !phases.has(value.phase as SentinelRecoveryPhase) ||
    (value.disposition !== "active" && value.disposition !== "manual_required" && value.disposition !== "rejected" &&
      value.disposition !== "delivered") ||
    !positiveInteger(value.state_version) || !isoTimestamp(value.created_at) || !isoTimestamp(value.updated_at) ||
    !(value.candidate_branch === null || nonEmpty(value.candidate_branch)) || !optionalSha(value.candidate_sha) ||
    !Array.isArray(value.changed_files) ||
    !value.changed_files.every((path) => nonEmpty(path) && !path.startsWith("/") && !path.includes("..")) ||
    !optionalSha(value.tree_sha) || !(value.failure_class === null || nonEmpty(value.failure_class)) ||
    !(value.failure_fingerprint === null ||
      (typeof value.failure_fingerprint === "string" && SHA256.test(value.failure_fingerprint))) ||
    !Array.isArray(value.artifact_ids) || !value.artifact_ids.every(positiveInteger) ||
    !Array.isArray(value.artifact_digests) ||
    !value.artifact_digests.every((digest) => typeof digest === "string" && /^sha256:[0-9a-f]{64}$/u.test(digest)) ||
    !(value.reason === null || nonEmpty(value.reason)) ||
    !(value.next_action === null || nonEmpty(value.next_action)) ||
    !(value.predecessor === null || nonEmpty(value.predecessor))
  ) throw new Error("Sentinel recovery record is invalid");
  const phase = value.phase as SentinelRecoveryPhase;
  const disposition = value.disposition as SentinelRecoveryDisposition;
  if (
    (terminalPhases.has(phase) && disposition !== phase) || (!terminalPhases.has(phase) && disposition !== "active")
  ) {
    throw new Error("Sentinel recovery record phase and disposition disagree");
  }
  if (phase === "checkpoint_durable" && (value.candidate_branch === null || value.candidate_sha === null)) {
    throw new Error("A durable Sentinel checkpoint requires a branch and commit SHA");
  }
  return value as SentinelRecoveryRecordV1;
};

export const assertSentinelRecoveryTransition = (
  previous: SentinelRecoveryRecordV1,
  next: SentinelRecoveryRecordV1,
): void => {
  parseSentinelRecoveryRecord(previous);
  parseSentinelRecoveryRecord(next);
  if (!sameIdentity(previous.identity, next.identity)) throw new Error("Sentinel recovery identity is immutable");
  if (previous.lease_token !== next.lease_token) {
    throw new Error("Sentinel recovery lease token changed without a new record");
  }
  if (next.state_version !== previous.state_version + 1) {
    throw new Error("Sentinel recovery state version must increase by one");
  }
  if (Date.parse(next.updated_at) < Date.parse(previous.updated_at)) {
    throw new Error("Sentinel recovery timestamp moved backwards");
  }
  if (!transitions[previous.phase].includes(next.phase)) {
    throw new Error(`Invalid Sentinel recovery transition: ${previous.phase} -> ${next.phase}`);
  }
};

export const parseSentinelReleaseRecord = (value: unknown): SentinelReleaseRecordV1 => {
  if (
    !isRecord(value) || value.schema_version !== 1 || typeof value.stable_sha !== "string" ||
    !FULL_SHA.test(value.stable_sha) ||
    !optionalSha(value.candidate_sha) || !Array.isArray(value.acceptance_evidence) ||
    !value.acceptance_evidence.every(nonEmpty) ||
    !isoTimestamp(value.activated_at) || !(value.rollback_reason === null || nonEmpty(value.rollback_reason)) ||
    !positiveInteger(value.generation)
  ) throw new Error("Sentinel release record is invalid");
  return value as SentinelReleaseRecordV1;
};
