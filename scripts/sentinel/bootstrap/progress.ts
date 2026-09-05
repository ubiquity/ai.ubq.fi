/**
 * Bootstrap progress decision (plan m06).
 *
 * The deterministic verdict is computed purely from canonical observations,
 * before any model call:
 *
 * - `stuck` when the current canonical state is unchanged from an earlier
 *   observation in the bounded window (identical consecutive states and
 *   cycles that return to a previous state are both obvious dead loops);
 * - `progress` only for durable verified advancement: a later verified
 *   phase/milestone, a new verified Git identity, a monotonic ledger advance
 *   tied to verification evidence, a verified generation advance, or a
 *   materially different corrective action followed by new evidence;
 * - `ambiguous` for anything else — changed but not provably advanced.
 *
 * Only `ambiguous` may reach the one injected zero-tool GPT-OSS classifier.
 * Any classifier absence, transport error, refusal, tool call, non-literal
 * output, or model mismatch resolves to `unknown`, which is fail-closed: a
 * decision other than `progress` never authorizes promotion and never
 * overrides health/rollback identity, exact Git SHA, or immutable revision
 * proof.
 *
 * This module is pure and performs no I/O.
 */

import {
  parseSentinelBootstrapClassifierEvidence,
  parseSentinelBootstrapProgressDecision,
  parseSentinelBootstrapProgressObservation,
  type SentinelBootstrapClassifierEvidenceV1,
  type SentinelBootstrapProgressDecisionV1,
  type SentinelBootstrapProgressObservationV1,
} from "./contracts.ts";
import { sentinelBootstrapObservationDigest, sentinelBootstrapProgressStateKey } from "./observation.ts";

/**
 * The fixed advisory classifier model, kept local in bootstrap so the package
 * module graph never reaches provider code. It is the zero-tool GPT-OSS model
 * used for ambiguous-progress advisory evidence only; it is never the
 * implementation model (Luna/max stays pinned in SENTINEL_BOOTSTRAP_POLICY).
 */
export const BOOTSTRAP_ADVISORY_CLASSIFIER_MODEL = "gpt-oss-120b";

/** Hard bound on how many observations one decision may consider. */
export const SENTINEL_BOOTSTRAP_PROGRESS_MAX_OBSERVATIONS = 16;
/** Bounded cycle-detection window (the plan's "bounded sequence"). */
export const SENTINEL_BOOTSTRAP_PROGRESS_WINDOW = 4;

const progressDecision = (
  verdict: SentinelBootstrapProgressDecisionV1["verdict"],
  reason: string,
  observations: readonly SentinelBootstrapProgressObservationV1[],
  now: string,
): SentinelBootstrapProgressDecisionV1 =>
  parseSentinelBootstrapProgressDecision({
    schema_version: 1,
    verdict,
    reason,
    observation_count: observations.length,
    state_digest: sentinelBootstrapObservationDigest(observations[observations.length - 1]!),
    classifier: null,
    resolved: verdict === "progress" ? "progress" : verdict === "stuck" ? "stuck" : "unknown",
    resolved_reason: verdict === "ambiguous" ? "classifier_not_provisioned" : reason,
    evaluated_at: now,
  });

/**
 * Deterministic durable-advancement evidence for one state transition.
 * Returns the canonical reason token or null when no rule fires.
 */
const durableProgressReason = (
  previous: SentinelBootstrapProgressObservationV1,
  current: SentinelBootstrapProgressObservationV1,
): string | null => {
  const verified = current.verification_evidence !== null;
  if (!verified) return null;
  if (previous.phase !== current.phase || previous.milestone !== current.milestone) {
    return "verified_phase_or_milestone_change";
  }
  if (current.git_sha !== null && previous.git_sha !== current.git_sha) {
    return "new_verified_git_identity";
  }
  if (
    previous.ledger_version !== null && current.ledger_version !== null &&
    current.ledger_version > previous.ledger_version
  ) {
    return "verified_ledger_advance";
  }
  if (
    previous.generation !== null && current.generation !== null &&
    current.generation > previous.generation
  ) {
    return "verified_generation_advance";
  }
  if (
    previous.failure_fingerprint !== null &&
    (current.failure_fingerprint === null || current.failure_fingerprint !== previous.failure_fingerprint)
  ) {
    return "corrective_action_with_new_evidence";
  }
  return null;
};

/**
 * Pure deterministic verdict over validated canonical observations, newest
 * last. An empty or over-long history is invalid input and throws; callers
 * fail closed by turning that into an absent progress decision.
 */
export const evaluateSentinelBootstrapProgress = (
  values: readonly unknown[],
  now: string,
): SentinelBootstrapProgressDecisionV1 => {
  if (values.length === 0) {
    throw new Error("Sentinel bootstrap progress requires at least one observation");
  }
  if (values.length > SENTINEL_BOOTSTRAP_PROGRESS_MAX_OBSERVATIONS) {
    throw new Error("Sentinel bootstrap progress observation history is too large");
  }
  const observations = values.map(parseSentinelBootstrapProgressObservation);
  const window = observations.slice(-SENTINEL_BOOTSTRAP_PROGRESS_WINDOW);
  const current = window[window.length - 1]!;
  const previous = window.length >= 2 ? window[window.length - 2]! : null;
  const currentKey = sentinelBootstrapProgressStateKey(current);
  if (window.slice(0, -1).some((observation) => sentinelBootstrapProgressStateKey(observation) === currentKey)) {
    return progressDecision("stuck", "unchanged_or_cycling_state", observations, now);
  }
  if (previous === null) {
    return progressDecision("ambiguous", "insufficient_history", observations, now);
  }
  const reason = durableProgressReason(previous, current);
  if (reason !== null) {
    return progressDecision("progress", reason, observations, now);
  }
  return progressDecision("ambiguous", "changed_state_without_verified_advancement", observations, now);
};

/** Fail-closed evidence for an injected classifier that threw. */
export const classifierFailureEvidence = (
  observation: SentinelBootstrapProgressObservationV1,
  reason: string,
  now: string,
): SentinelBootstrapClassifierEvidenceV1 =>
  parseSentinelBootstrapClassifierEvidence({
    schema_version: 1,
    answer: "unknown",
    raw: null,
    reason,
    requested_model: BOOTSTRAP_ADVISORY_CLASSIFIER_MODEL,
    status: null,
    requested_at: now,
    observation_digest: sentinelBootstrapObservationDigest(observation),
    advisory: true,
  });

/**
 * Resolves the deterministic decision with the optional classifier evidence.
 * The classifier is consulted only for `ambiguous`; every other path keeps
 * the deterministic outcome and records no classifier evidence. Missing or
 * unknown evidence always resolves to `unknown` (fail-closed).
 */
export const resolveSentinelBootstrapProgress = (
  decision: SentinelBootstrapProgressDecisionV1,
  classifier: SentinelBootstrapClassifierEvidenceV1 | null,
): SentinelBootstrapProgressDecisionV1 => {
  if (decision.verdict !== "ambiguous") {
    return { ...decision, classifier: null, resolved: decision.verdict, resolved_reason: decision.reason };
  }
  if (classifier === null) {
    return { ...decision, classifier: null, resolved: "unknown", resolved_reason: "classifier_not_provisioned" };
  }
  if (classifier.answer === "true") {
    return { ...decision, classifier, resolved: "progress", resolved_reason: "classifier_true" };
  }
  if (classifier.answer === "false") {
    return { ...decision, classifier, resolved: "stuck", resolved_reason: "classifier_false" };
  }
  return { ...decision, classifier, resolved: "unknown", resolved_reason: classifier.reason };
};
