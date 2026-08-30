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
  ledger: SentinelRecoveryLedgerV1;
  repository: string;
  source_kind: SentinelRecoverySourceKind;
  source_id: string;
  source_revision: string;
  now: string;
}>;

export type SentinelRecoverySelectionResult = Readonly<{
  related_records: readonly SentinelRecoveryRecordV1[];
  current_record: SentinelRecoveryRecordV1 | null;
  current_identity_key: string | null;
  retry_decision: SentinelRetryDecision | null;
  retry_is_due: boolean;
  next_generation: number;
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
  const current =
    related.find((record) =>
      record.identity.source_revision === input.source_revision && record.disposition === "active"
    ) ?? null;
  const currentKey = current === null ? null : sentinelRecoveryIdentityKey(current.identity);
  const retryDecision = currentKey === null
    ? null
    : ledger.retry_decisions.find((entry) => entry.identity_key === currentKey)?.decision ?? null;
  const retryIsDue = current?.phase === "retry_wait" && retryDecision?.should_retry === true &&
    retryDecision.retry_at !== null && Date.parse(retryDecision.retry_at) <= Date.parse(input.now);
  const nextGeneration = related.reduce(
    (maximum, record) => Math.max(maximum, record.identity.candidate_generation),
    0,
  ) + 1;
  return {
    related_records: related,
    current_record: current,
    current_identity_key: currentKey,
    retry_decision: retryDecision,
    retry_is_due: retryIsDue,
    next_generation: nextGeneration,
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
    const retainedTerminal = records
      .filter((candidate) => isTerminalRecoveryPhase(candidate.phase))
      .sort((left, right) => {
        const byTime = Date.parse(right.updated_at) - Date.parse(left.updated_at);
        return byTime !== 0
          ? byTime
          : sentinelRecoveryIdentityKey(right.identity).localeCompare(sentinelRecoveryIdentityKey(left.identity));
      })
      .slice(0, SENTINEL_RECOVERY_LEDGER_MAX_RECORDS - active.length);
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
