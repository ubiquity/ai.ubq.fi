import { isRecord } from "../../src/utils.ts";
import {
  assertSentinelRecoveryTransition,
  isTerminalRecoveryPhase,
  parseSentinelRecoveryRecord,
  type SentinelRecoveryIdentityV1,
  type SentinelRecoveryRecordV1,
} from "./recovery.ts";
import {
  parseSentinelRetryAttempt,
  parseSentinelRetryDecision,
  type SentinelRetryAttemptV1,
  type SentinelRetryDecision,
} from "./retry.ts";
import type { SentinelRecoverySourceKind } from "./recovery.ts";

export const SENTINEL_RECOVERY_LEDGER_SCHEMA_VERSION = 1 as const;
export const SENTINEL_RECOVERY_LEDGER_PATH = "docs/sentinel-recovery-records.json";
export const SENTINEL_RECOVERY_LEDGER_MAX_RECORDS = 512;
export const SENTINEL_RECOVERY_LEDGER_MAX_RETRY_ATTEMPTS = 4_096;

export type SentinelRecoveryLeaseV1 = Readonly<{
  identity_key: string;
  owner: string;
  token: string;
  expires_at: string;
}>;

export type SentinelRecoveryLedgerV1 = Readonly<{
  schema_version: typeof SENTINEL_RECOVERY_LEDGER_SCHEMA_VERSION;
  records: readonly SentinelRecoveryRecordV1[];
  retry_history: readonly SentinelRetryAttemptV1[];
  retry_decisions: readonly Readonly<{ identity_key: string; decision: SentinelRetryDecision }>[];
  leases: readonly SentinelRecoveryLeaseV1[];
}>;

const validTimestamp = (value: unknown): value is string =>
  typeof value === "string" && Number.isFinite(Date.parse(value)) && /^\d{4}-\d{2}-\d{2}T/u.test(value);

export const sentinelRecoveryIdentityKey = (identity: SentinelRecoveryIdentityV1): string =>
  JSON.stringify([
    identity.repository,
    identity.source_kind,
    identity.source_id,
    identity.source_revision,
    identity.candidate_generation,
  ]);

const parseLease = (value: unknown): SentinelRecoveryLeaseV1 => {
  if (
    !isRecord(value) || typeof value.identity_key !== "string" || value.identity_key.length === 0 ||
    typeof value.owner !== "string" || value.owner.trim().length === 0 ||
    typeof value.token !== "string" || value.token.trim().length === 0 || !validTimestamp(value.expires_at)
  ) throw new Error("Sentinel recovery lease is invalid");
  return value as SentinelRecoveryLeaseV1;
};

export const parseSentinelRecoveryLedger = (value: unknown): SentinelRecoveryLedgerV1 => {
  if (
    !isRecord(value) || value.schema_version !== SENTINEL_RECOVERY_LEDGER_SCHEMA_VERSION ||
    !Array.isArray(value.records) || value.records.length > SENTINEL_RECOVERY_LEDGER_MAX_RECORDS ||
    !Array.isArray(value.retry_history) ||
    value.retry_history.length > SENTINEL_RECOVERY_LEDGER_MAX_RETRY_ATTEMPTS ||
    !Array.isArray(value.retry_decisions) || !Array.isArray(value.leases)
  ) throw new Error("Sentinel recovery ledger is invalid");
  const records = value.records.map(parseSentinelRecoveryRecord);
  const retryHistory = value.retry_history.map(parseSentinelRetryAttempt);
  const retryDecisions = value.retry_decisions.map((entry) => {
    if (!isRecord(entry) || typeof entry.identity_key !== "string" || entry.identity_key.length === 0) {
      throw new Error("Sentinel retry decision entry is invalid");
    }
    return Object.freeze({ identity_key: entry.identity_key, decision: parseSentinelRetryDecision(entry.decision) });
  });
  const leases = value.leases.map(parseLease);
  const recordKeys = records.map((record) => sentinelRecoveryIdentityKey(record.identity));
  const decisionKeys = retryDecisions.map((entry) => entry.identity_key);
  const leaseKeys = leases.map((lease) => lease.identity_key);
  if (
    new Set(recordKeys).size !== recordKeys.length || new Set(decisionKeys).size !== decisionKeys.length ||
    new Set(leaseKeys).size !== leaseKeys.length
  ) {
    throw new Error("Sentinel recovery ledger contains duplicate identities");
  }
  if (
    retryDecisions.some((entry) => !recordKeys.includes(entry.identity_key)) ||
    leases.some((lease) => !recordKeys.includes(lease.identity_key))
  ) {
    throw new Error("Sentinel recovery metadata has no matching record");
  }
  return Object.freeze({
    schema_version: SENTINEL_RECOVERY_LEDGER_SCHEMA_VERSION,
    records: Object.freeze(records),
    retry_history: Object.freeze(retryHistory),
    retry_decisions: Object.freeze(retryDecisions),
    leases: Object.freeze(leases),
  });
};

export const emptySentinelRecoveryLedger = (): SentinelRecoveryLedgerV1 =>
  parseSentinelRecoveryLedger({ schema_version: 1, records: [], retry_history: [], retry_decisions: [], leases: [] });

export const renderSentinelRecoveryLedger = (ledger: SentinelRecoveryLedgerV1): string =>
  `${JSON.stringify(parseSentinelRecoveryLedger(ledger), null, 2)}\n`;

export const nonTerminalSentinelRecoveryRecords = (
  ledger: SentinelRecoveryLedgerV1,
): readonly SentinelRecoveryRecordV1[] =>
  parseSentinelRecoveryLedger(ledger).records.filter((record) => !isTerminalRecoveryPhase(record.phase));

export type SentinelRecoverySelectionInput = Readonly<{
  /** Parsed and validated on every call; malformed data fails closed. */
  ledger: unknown;
  repository: string;
  source_kind: SentinelRecoverySourceKind;
  source_id: string;
  source_revision: string;
  now: string;
  /**
   * The exact active recovery record this run may continue (matrix
   * convergence). Authorization is caller-bound: the record must match on
   * identity key, run_id, and lease_token so an arbitrary identity key or a
   * competitor's lease can never authorize a continuation.
   */
  continuation_record?: SentinelRecoveryRecordV1 | null;
}>;

export type SentinelRecoveryEligibilityReason =
  | "fresh_source"
  | "active_retry_due"
  | "active_continuation"
  | "terminal_delivered"
  | "terminal_manual_required"
  | "terminal_rejected"
  | "active_unavailable"
  | "active_collision"
  | "active_lease_held";

export type SentinelRecoveryEligibility = Readonly<{
  available: boolean;
  reason: SentinelRecoveryEligibilityReason;
  blocking_record: SentinelRecoveryRecordV1 | null;
}>;

/** The shared recovery-state context one selection stage consumes. */
export type SentinelRecoveryEligibilityContext = Readonly<{
  repository: string;
  ledger: SentinelRecoveryLedgerV1;
  now: string;
  continuation_record?: SentinelRecoveryRecordV1 | null;
}>;

export type SentinelRecoverySelectionResult = Readonly<{
  related_records: readonly SentinelRecoveryRecordV1[];
  current_record: SentinelRecoveryRecordV1 | null;
  current_identity_key: string | null;
  retry_decision: SentinelRetryDecision | null;
  retry_is_due: boolean;
  next_generation: number;
  /**
   * Deterministic eligibility for the exact source revision: whether a new
   * generation may be claimed. Terminal delivered/manual_required/rejected
   * decisions for the unchanged revision block it; an active record proceeds
   * only through its own due retry decision (or this run's exact continuation
   * identity); a truly unseen revision stays eligible.
   */
  eligibility: SentinelRecoveryEligibility;
}>;

/**
 * The exact selection/recovery lookup the Sentinel cycle runs after a work
 * item (issue, backlog entry, or matrix cell) is chosen. It returns the
 * durable record for the authoritative source revision, the durable retry
 * decision, whether the bounded retry delay elapsed, and the next candidate
 * generation. A recovered canonical parent record (linked through
 * `predecessor` to its exact cell record) is matched here, so a next-cycle
 * selection sees and defers to the recovered checkpoint instead of claiming a
 * fresh generation.
 */
export const resolveSentinelRecoverySelection = (
  input: SentinelRecoverySelectionInput,
): SentinelRecoverySelectionResult => {
  const ledger = parseSentinelRecoveryLedger(input.ledger);
  if (
    input.repository.trim().length === 0 || input.source_id.trim().length === 0 ||
    input.source_revision.trim().length === 0 || !Number.isFinite(Date.parse(input.now))
  ) throw new Error("Sentinel recovery selection input is invalid");
  const related = ledger.records.filter((record) =>
    record.identity.repository === input.repository &&
    record.identity.source_kind === input.source_kind &&
    record.identity.source_id === input.source_id
  );
  const exactRevision = related.filter((record) => record.identity.source_revision === input.source_revision);
  const activeExact = exactRevision.filter((record) => record.disposition === "active");
  const terminalExact = exactRevision.filter((record) => isTerminalRecoveryPhase(record.phase));
  const retryDecisionFor = (record: SentinelRecoveryRecordV1): SentinelRetryDecision | null =>
    ledger.retry_decisions.find((entry) => entry.identity_key === sentinelRecoveryIdentityKey(record.identity))
      ?.decision ??
      null;
  const retryIsDueFor = (record: SentinelRecoveryRecordV1): boolean => {
    const decision = retryDecisionFor(record);
    return record.phase === "retry_wait" && decision?.should_retry === true && decision.retry_at !== null &&
      Date.parse(decision.retry_at) <= Date.parse(input.now);
  };
  // A live lease with another owner's token means that exact record is being
  // worked right now or is entering a bounded pass; it is not claimable until
  // the lease expires or its owner releases it.
  const leasedByOther = (record: SentinelRecoveryRecordV1): boolean => {
    const key = sentinelRecoveryIdentityKey(record.identity);
    const lease = ledger.leases.find((candidate) => candidate.identity_key === key) ?? null;
    return lease !== null && lease.token !== record.lease_token &&
      Date.parse(lease.expires_at) > Date.parse(input.now);
  };
  const continuationRecord = input.continuation_record ?? null;
  const continuation = continuationRecord === null
    ? null
    : activeExact.find((record) =>
      sentinelRecoveryIdentityKey(record.identity) ===
        sentinelRecoveryIdentityKey(continuationRecord.identity) &&
      record.run_id === continuationRecord.run_id && record.lease_token === continuationRecord.lease_token
    ) ?? null;
  // Ambiguous multiple-active state (the pre-repair bypass could start one
  // generation while an earlier one still held the same source revision) never
  // silently picks a winner; it stays unavailable unless the run's own exact
  // continuation record is bound. The current record is deterministic for
  // reporting only (newest generation, then newest update).
  const newestActive = activeExact.length === 0
    ? null
    : activeExact.reduce((newest, candidate) =>
      candidate.identity.candidate_generation > newest.identity.candidate_generation ||
        (candidate.identity.candidate_generation === newest.identity.candidate_generation &&
          Date.parse(candidate.updated_at) > Date.parse(newest.updated_at))
        ? candidate
        : newest
    );
  const current = continuation ?? newestActive;
  const currentKey = current === null ? null : sentinelRecoveryIdentityKey(current.identity);
  const retryDecision = current === null ? null : retryDecisionFor(current);
  const retryIsDue = current === null ? false : retryIsDueFor(current);
  const nextGeneration = related.reduce(
    (maximum, record) => Math.max(maximum, record.identity.candidate_generation),
    0,
  ) + 1;
  let eligibility: SentinelRecoveryEligibility;
  if (continuation !== null) {
    eligibility = leasedByOther(continuation)
      ? { available: false, reason: "active_lease_held", blocking_record: continuation }
      : { available: true, reason: "active_continuation", blocking_record: null };
  } else if (terminalExact.length > 0) {
    const blocking = terminalExact.reduce((newest, candidate) =>
      Date.parse(candidate.updated_at) > Date.parse(newest.updated_at) ||
        (Date.parse(candidate.updated_at) === Date.parse(newest.updated_at) &&
          sentinelRecoveryIdentityKey(candidate.identity).localeCompare(
              sentinelRecoveryIdentityKey(newest.identity),
            ) > 0)
        ? candidate
        : newest
    );
    const reason: SentinelRecoveryEligibilityReason = blocking.disposition === "delivered"
      ? "terminal_delivered"
      : blocking.disposition === "manual_required"
      ? "terminal_manual_required"
      : "terminal_rejected";
    eligibility = { available: false, reason, blocking_record: blocking };
  } else if (activeExact.length > 1) {
    eligibility = { available: false, reason: "active_collision", blocking_record: newestActive };
  } else if (activeExact.length === 1) {
    const sole = activeExact[0]!;
    if (leasedByOther(sole)) {
      eligibility = { available: false, reason: "active_lease_held", blocking_record: sole };
    } else {
      eligibility = retryIsDueFor(sole)
        ? { available: true, reason: "active_retry_due", blocking_record: null }
        : { available: false, reason: "active_unavailable", blocking_record: sole };
    }
  } else {
    eligibility = { available: true, reason: "fresh_source", blocking_record: null };
  }
  return {
    related_records: related,
    current_record: current,
    current_identity_key: currentKey,
    retry_decision: retryDecision,
    retry_is_due: retryIsDue,
    next_generation: nextGeneration,
    eligibility,
  };
};

export const upsertSentinelRecoveryRecord = (
  ledgerValue: unknown,
  recordValue: unknown,
  expectedStateVersion: number | null,
): SentinelRecoveryLedgerV1 => {
  const ledger = parseSentinelRecoveryLedger(ledgerValue);
  const record = parseSentinelRecoveryRecord(recordValue);
  const key = sentinelRecoveryIdentityKey(record.identity);
  const existing = ledger.records.find((candidate) => sentinelRecoveryIdentityKey(candidate.identity) === key) ?? null;
  if (expectedStateVersion === null) {
    if (existing !== null) throw new Error("Sentinel recovery record already exists");
  } else if (existing === null || existing.state_version !== expectedStateVersion) {
    throw new Error("Sentinel recovery record compare-and-swap failed");
  }
  if (existing !== null) assertSentinelRecoveryTransition(existing, record);
  let records = [
    ...ledger.records.filter((candidate) => sentinelRecoveryIdentityKey(candidate.identity) !== key),
    record,
  ]
    .sort((left, right) =>
      sentinelRecoveryIdentityKey(left.identity).localeCompare(sentinelRecoveryIdentityKey(right.identity))
    );
  if (records.length > SENTINEL_RECOVERY_LEDGER_MAX_RECORDS) {
    const active = records.filter((candidate) => !isTerminalRecoveryPhase(candidate.phase));
    if (active.length > SENTINEL_RECOVERY_LEDGER_MAX_RECORDS) {
      throw new Error("Sentinel recovery ledger has too many active records");
    }
    const terminal = records
      .filter((candidate) => isTerminalRecoveryPhase(candidate.phase))
      .sort((left, right) => {
        const byTime = Date.parse(right.updated_at) - Date.parse(left.updated_at);
        return byTime !== 0
          ? byTime
          : sentinelRecoveryIdentityKey(right.identity).localeCompare(sentinelRecoveryIdentityKey(left.identity));
      });
    const capacity = SENTINEL_RECOVERY_LEDGER_MAX_RECORDS - active.length;
    // One newest terminal circuit decision per exact issue/review source
    // revision is required for unchanged-source eligibility; evicting it would
    // silently reopen already-decided work. Unrelated terminal records (other
    // source kinds or superseded generations of the same revision) are not
    // protected and are retained only by recency. If the required protected
    // records cannot fit, fail closed instead of dropping a circuit decision.
    const required: SentinelRecoveryRecordV1[] = [];
    const protectedTuples = new Set<string>();
    for (const candidate of terminal) {
      if (
        candidate.identity.source_kind !== "github_issue" &&
        candidate.identity.source_kind !== "review_backlog"
      ) continue;
      const tuple = JSON.stringify([
        candidate.identity.repository,
        candidate.identity.source_kind,
        candidate.identity.source_id,
        candidate.identity.source_revision,
      ]);
      if (protectedTuples.has(tuple)) continue;
      protectedTuples.add(tuple);
      required.push(candidate);
    }
    if (required.length > capacity) {
      throw new Error("Sentinel recovery ledger cannot retain required terminal circuit decisions");
    }
    const requiredKeys = new Set(required.map((candidate) => sentinelRecoveryIdentityKey(candidate.identity)));
    const retainedTerminal = [
      ...required,
      ...terminal.filter((candidate) => !requiredKeys.has(sentinelRecoveryIdentityKey(candidate.identity))),
    ].slice(0, capacity);
    records = [...active, ...retainedTerminal].sort((left, right) =>
      sentinelRecoveryIdentityKey(left.identity).localeCompare(sentinelRecoveryIdentityKey(right.identity))
    );
  }
  const retainedKeys = new Set(records.map((candidate) => sentinelRecoveryIdentityKey(candidate.identity)));
  return parseSentinelRecoveryLedger({
    ...ledger,
    records,
    retry_history: ledger.retry_history.filter((attempt) =>
      retainedKeys.has(sentinelRecoveryIdentityKey(attempt.identity))
    ),
    retry_decisions: ledger.retry_decisions.filter((entry) => retainedKeys.has(entry.identity_key)),
    leases: ledger.leases.filter((lease) => retainedKeys.has(lease.identity_key)),
  });
};

export const acquireSentinelRecoveryLease = (
  ledgerValue: unknown,
  input: Readonly<
    { identity: SentinelRecoveryIdentityV1; owner: string; token: string; now: string; expires_at: string }
  >,
): SentinelRecoveryLedgerV1 => {
  const ledger = parseSentinelRecoveryLedger(ledgerValue);
  if (
    !validTimestamp(input.now) || !validTimestamp(input.expires_at) ||
    Date.parse(input.expires_at) <= Date.parse(input.now)
  ) {
    throw new Error("Sentinel recovery lease interval is invalid");
  }
  const key = sentinelRecoveryIdentityKey(input.identity);
  if (!ledger.records.some((record) => sentinelRecoveryIdentityKey(record.identity) === key)) {
    throw new Error("Sentinel recovery lease has no matching record");
  }
  const existing = ledger.leases.find((lease) => lease.identity_key === key);
  if (existing && existing.token !== input.token && Date.parse(existing.expires_at) > Date.parse(input.now)) {
    throw new Error("Sentinel recovery record is leased by another owner");
  }
  const lease = parseLease({
    identity_key: key,
    owner: input.owner,
    token: input.token,
    expires_at: input.expires_at,
  });
  return parseSentinelRecoveryLedger({
    ...ledger,
    leases: [...ledger.leases.filter((candidate) => candidate.identity_key !== key), lease]
      .sort((left, right) => left.identity_key.localeCompare(right.identity_key)),
  });
};

export const releaseSentinelRecoveryLease = (
  ledgerValue: unknown,
  identity: SentinelRecoveryIdentityV1,
  token: string,
): SentinelRecoveryLedgerV1 => {
  const ledger = parseSentinelRecoveryLedger(ledgerValue);
  const key = sentinelRecoveryIdentityKey(identity);
  const existing = ledger.leases.find((lease) => lease.identity_key === key);
  if (!existing) return ledger;
  if (existing.token !== token) throw new Error("Sentinel recovery lease token does not match");
  return parseSentinelRecoveryLedger({
    ...ledger,
    leases: ledger.leases.filter((lease) => lease.identity_key !== key),
  });
};
