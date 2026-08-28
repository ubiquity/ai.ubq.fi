import {
  AUTHORITATIVE_BOOTSTRAP_FAILURE_CLASSES,
  isAuthoritativeBootstrapFailureClass,
  isTransientBootstrapFailureClass,
  parseSentinelBootstrapHealthSignal,
  type SentinelBootstrapFailureClass,
  type SentinelBootstrapHealthSignalV1,
} from "./contracts.ts";
import { assertPositiveGeneration } from "./contracts.ts";
import { SENTINEL_BOOTSTRAP_POLICY } from "./policy.ts";

export const BOOTSTRAP_REPEATED_FAILURE_THRESHOLD = SENTINEL_BOOTSTRAP_POLICY.repeatedFailureThreshold;

export type SentinelBootstrapHealthDecision = Readonly<{
  status: "healthy" | "transient" | "unhealthy";
  rollback: boolean;
  generation: number;
  failure_class: SentinelBootstrapFailureClass | null;
  failure_fingerprint: string | null;
  evidence_refs: readonly string[];
  reason:
    | "no_failure_observed"
    | "transient_failure_observed"
    | "authoritative_failure_threshold_not_reached"
    | "authoritative_failure_repeated";
}>;

type HealthInput = Readonly<{
  activeGeneration: number;
  signals: readonly unknown[];
}>;

const unique = <T>(values: readonly T[]): readonly T[] => [...new Set(values)];

const observedOrder = (left: SentinelBootstrapHealthSignalV1, right: SentinelBootstrapHealthSignalV1): number => {
  const byTime = Date.parse(left.observed_at) - Date.parse(right.observed_at);
  if (byTime !== 0) return byTime;
  const byFingerprint = left.failure_fingerprint.localeCompare(right.failure_fingerprint);
  if (byFingerprint !== 0) return byFingerprint;
  const byClass = left.failure_class.localeCompare(right.failure_class);
  if (byClass !== 0) return byClass;
  return observationIdentity(left).localeCompare(observationIdentity(right));
};

const failureEvidence = (signals: readonly SentinelBootstrapHealthSignalV1[]): readonly string[] =>
  unique(
    [...signals]
      .sort(observedOrder)
      .flatMap((signal) => signal.evidence_refs),
  ).slice(0, 8);

/**
 * Observations normally carry a workflow-run identity. Fixtures and a few
 * older producers do not, so derive a stable identity from the immutable
 * signal fields instead of counting the same observation once per delivery.
 */
const observationIdentity = (signal: SentinelBootstrapHealthSignalV1): string =>
  signal.observation_id ?? [
    signal.failure_class,
    signal.failure_fingerprint,
    signal.observed_at,
    ...signal.evidence_refs,
  ].join("\u001f");

const decision = (
  status: SentinelBootstrapHealthDecision["status"],
  rollback: boolean,
  generation: number,
  failureClass: SentinelBootstrapFailureClass | null,
  fingerprint: string | null,
  evidenceRefs: readonly string[],
  reason: SentinelBootstrapHealthDecision["reason"],
): SentinelBootstrapHealthDecision => ({
  status,
  rollback,
  generation,
  failure_class: failureClass,
  failure_fingerprint: fingerprint,
  evidence_refs: evidenceRefs,
  reason,
});

/**
 * Evaluate only facts observed for the current activation generation. A
 * stale run cannot make a newer generation roll back. Provider, transport,
 * HTTP 5xx, and Luna capacity observations are deliberately non-rollback
 * signals, regardless of how often they occur.
 */
export const evaluateSentinelBootstrapHealth = (
  input: HealthInput,
): SentinelBootstrapHealthDecision => {
  const generation = assertPositiveGeneration(input.activeGeneration, "Sentinel bootstrap generation");
  const signals = input.signals.map(parseSentinelBootstrapHealthSignal);
  const current = signals.filter((signal) => signal.generation === generation);
  if (current.length === 0) {
    return decision("healthy", false, generation, null, null, [], "no_failure_observed");
  }

  const authoritative = current.filter((signal) => isAuthoritativeBootstrapFailureClass(signal.failure_class));
  const direct = authoritative.filter((signal) => signal.failure_class !== "workflow_failure");
  if (direct.length > 0) {
    const first = [...direct].sort(observedOrder)[0]!;
    return decision(
      "unhealthy",
      true,
      generation,
      first.failure_class,
      first.failure_fingerprint,
      failureEvidence(direct),
      "authoritative_failure_repeated",
    );
  }

  const workflowByFingerprint = new Map<string, SentinelBootstrapHealthSignalV1[]>();
  for (const signal of authoritative) {
    if (signal.failure_class !== "workflow_failure") continue;
    const observations = workflowByFingerprint.get(signal.failure_fingerprint) ?? [];
    observations.push(signal);
    workflowByFingerprint.set(signal.failure_fingerprint, observations);
  }
  const repeated = [...workflowByFingerprint.entries()]
    .map(([fingerprint, observations]) => ({ fingerprint, observations }))
    .filter(({ observations }) => {
      return new Set(observations.map(observationIdentity)).size >= BOOTSTRAP_REPEATED_FAILURE_THRESHOLD;
    })
    .sort((left, right) => left.fingerprint.localeCompare(right.fingerprint))[0];
  if (repeated) {
    return decision(
      "unhealthy",
      true,
      generation,
      "workflow_failure",
      repeated.fingerprint,
      failureEvidence(repeated.observations),
      "authoritative_failure_repeated",
    );
  }

  const transient = current.filter((signal) => isTransientBootstrapFailureClass(signal.failure_class));
  if (transient.length > 0) {
    return decision(
      "transient",
      false,
      generation,
      [...transient].sort(observedOrder)[0]!.failure_class,
      [...transient].sort(observedOrder)[0]!.failure_fingerprint,
      failureEvidence(transient),
      authoritative.length > 0 ? "authoritative_failure_threshold_not_reached" : "transient_failure_observed",
    );
  }

  const pending = [...authoritative].sort(observedOrder)[0];
  return decision(
    "transient",
    false,
    generation,
    pending?.failure_class ?? null,
    pending?.failure_fingerprint ?? null,
    failureEvidence(authoritative),
    "authoritative_failure_threshold_not_reached",
  );
};

export const isRollbackEligible = (decision: SentinelBootstrapHealthDecision): boolean => decision.rollback;

export const isBootstrapFailureClass = (value: unknown): value is SentinelBootstrapFailureClass =>
  typeof value === "string" &&
  (AUTHORITATIVE_BOOTSTRAP_FAILURE_CLASSES.includes(value as SentinelBootstrapFailureClass) ||
    isTransientBootstrapFailureClass(value));
