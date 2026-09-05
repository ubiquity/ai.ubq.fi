/**
 * Bounded advisory summary of the evolving recovery ledger (plan m06).
 *
 * Bootstrap needs exactly three facts from `docs/sentinel-recovery-records.json`
 * for progress reporting: the schema version, the highest `state_version`
 * observed across records, and whether any retry history exists. This module
 * parses only those fields against the actual recovery ledger v1 shape with
 * its own local bounded checks, so bootstrap never imports the evolving
 * recovery/retry validators or their provider-adjacent dependency graph.
 *
 * The summary is advisory evidence only. Absent or malformed ledger metadata
 * yields `null` — it never authorizes acceptance, recovery, dispatch, or any
 * rollback decision on its own.
 */

export const SENTINEL_RECOVERY_LEDGER_PATH = "docs/sentinel-recovery-records.json";

const SENTINEL_RECOVERY_LEDGER_MAX_RECORDS = 512;
const SENTINEL_RECOVERY_LEDGER_MAX_RETRY_ATTEMPTS = 4_096;

export type SentinelRecoveryLedgerSummary = Readonly<{
  /** Highest record state version; 0 when the ledger holds no records. */
  max_state_version: number;
  retry_state: "retrying" | "none";
}>;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const positiveInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

/**
 * Parses the advisory progress fields only. The actual ledger shape is
 * `{ schema_version, records, retry_history, retry_decisions, leases }`; the
 * last two arrays are checked for presence (they are always arrays in the
 * real v1 shape) but not deep-validated or bounded because bootstrap never
 * reads them. Any deviation from the shape returns null (fail closed) instead
 * of importing an evolving validator.
 */
export const parseAdvisoryRecoveryLedgerSummary = (value: unknown): SentinelRecoveryLedgerSummary | null => {
  if (
    !isRecord(value) || value.schema_version !== 1 ||
    !Array.isArray(value.records) || value.records.length > SENTINEL_RECOVERY_LEDGER_MAX_RECORDS ||
    !Array.isArray(value.retry_history) || value.retry_history.length > SENTINEL_RECOVERY_LEDGER_MAX_RETRY_ATTEMPTS ||
    !Array.isArray(value.retry_decisions) || !Array.isArray(value.leases)
  ) return null;
  for (const record of value.records) {
    if (!isRecord(record) || !positiveInteger(record.state_version)) return null;
  }
  for (const attempt of value.retry_history) {
    if (!isRecord(attempt)) return null;
  }
  const maxStateVersion = value.records.reduce(
    (maximum, record) => Math.max(maximum, record.state_version as number),
    0,
  );
  return Object.freeze({
    max_state_version: maxStateVersion,
    retry_state: value.retry_history.length > 0 ? "retrying" : "none",
  });
};
