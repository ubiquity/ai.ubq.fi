/**
 * Canonical per-run progress observations (plan m06).
 *
 * An observation is a bounded snapshot of one run: run/source identity,
 * generation, phase/milestone, failure fingerprint, Git SHA, ledger/state
 * version where available, retry state, and verification evidence. Only the
 * field values form the canonical durable state; `run_id`/`source` identify
 * the observation and never mask an identical dead state.
 *
 * This module is pure and performs no I/O.
 */

import type { SentinelBootstrapProgressObservationV1 } from "./contracts.ts";

/**
 * The bounded classifier data shape projected from a canonical observation.
 * Defined locally so bootstrap's module graph never reaches provider code;
 * it mirrors the classifier adapter contract that lives outside the package.
 */
export type BootstrapObservation = Readonly<{
  runId: string;
  generation: number | null;
  phase: string | null;
  milestone: string | null;
  failureFingerprint: string | null;
  gitSha: string | null;
  ledgerVersion: number | null;
  retryState: string | null;
  verificationEvidence: string | null;
}>;

/**
 * The durable state fields compared for cycle detection. Order is fixed so
 * two observations of the same state always produce the same key.
 */
export const sentinelBootstrapProgressStateKey = (
  observation: SentinelBootstrapProgressObservationV1,
): string =>
  JSON.stringify([
    observation.source,
    observation.generation,
    observation.phase,
    observation.milestone,
    observation.failure_fingerprint,
    observation.git_sha,
    observation.ledger_version,
    observation.retry_state,
    observation.verification_evidence,
  ]);

/** Deterministic FNV-1a 64-bit advisory digest (hex, bounded, no I/O). */
export const sentinelBootstrapAdvisoryDigest = (value: unknown): string => {
  let hash = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(JSON.stringify(value))) {
    hash ^= BigInt(byte);
    hash = (hash * 0x100000001b3n) & 0xffffffffffffffffn;
  }
  return hash.toString(16).padStart(16, "0");
};

/**
 * Projects a canonical observation onto the bounded classifier data shape.
 * The object is built in fixed key order so its digest is deterministic.
 */
export const toBootstrapClassifierObservation = (
  observation: SentinelBootstrapProgressObservationV1,
): BootstrapObservation => ({
  runId: observation.run_id,
  generation: observation.generation,
  phase: observation.phase,
  milestone: observation.milestone,
  failureFingerprint: observation.failure_fingerprint,
  gitSha: observation.git_sha,
  ledgerVersion: observation.ledger_version,
  retryState: observation.retry_state,
  verificationEvidence: observation.verification_evidence,
});

/** Advisory digest binding classifier evidence to the observation sent. */
export const sentinelBootstrapObservationDigest = (
  observation: SentinelBootstrapProgressObservationV1,
): string => sentinelBootstrapAdvisoryDigest(toBootstrapClassifierObservation(observation));
