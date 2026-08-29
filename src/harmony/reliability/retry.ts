/**
 * Retry policy for the reliability harness (plan m05).
 *
 * Deterministic rule set that decides whether re-executing the SAME tool call
 * (identical tool + identical canonical arguments) is allowed:
 *
 * - **Transient codes** (`timeout`, `internal`, `unavailable`, `transport`)
 *   may be retried up to `maxRetriesPerCall` times with a fixed backoff.
 * - **Deterministic codes** (`invalid_args`, `path_escape`, `write_scope`,
 *   `not_found`, `patch_failed`, `exec_failed`) are never retried with
 *   identical arguments: the same call in the same workspace yields the same
 *   result, so re-execution is a duplicate action.  The model must change
 *   arguments or approach — the harness converts the follow-up into
 *   `repeated_failure` feedback instead of re-running it.
 *
 * The policy is pure configuration (no environment variables, no CLI flags);
 * the default is conservative and the code set is open for tests.
 */

import type { ToolErrorCode, ToolResult } from "../tools/result.ts";
import { digestShort } from "./hash.ts";

/** Harness-level transient codes (transport is owned by m05, not m04). */
export const TRANSPORT_ERROR_CODE = "transport";

export type RetryableCode = ToolErrorCode | typeof TRANSPORT_ERROR_CODE;

export interface RetryPolicy {
  /** Maximum retries of one identical call after a failure; default 1. */
  maxRetriesPerCall: number;
  /** Fixed delay before each retry, milliseconds; default 100. */
  backoffMs: number;
  /** Codes that allow an identical-call retry. */
  retryableCodes: readonly RetryableCode[];
}

export const DEFAULT_RETRY_POLICY: RetryPolicy = {
  maxRetriesPerCall: 1,
  backoffMs: 100,
  retryableCodes: ["timeout", "internal", "unavailable", "transport"],
};

export const DEFAULT_DETERMINISTIC_CODES: readonly ToolErrorCode[] = [
  "invalid_args",
  "path_escape",
  "write_scope",
  "not_found",
  "patch_failed",
  "exec_failed",
];

export type RetryDecision = Readonly<
  | { retry: true; delayMs: number; attempt: number; code: string; reason: "transient_code" }
  | { retry: false; delayMs: 0; attempt: number; code: string | null; reason: "not_retryable" | "attempts_exhausted" }
>;

/**
 * Decides whether one identical call may be retried.
 *
 * @param code         error code of the previous failure (null when unknown)
 * @param priorAttempts number of prior attempts of this identical call
 */
export const decideRetry = (policy: RetryPolicy, code: string | null, priorAttempts: number): RetryDecision => {
  const attempt = priorAttempts + 1;
  // A null code means the previous identical call SUCCEEDED: re-executing it
  // is a duplicate action, never a retry.
  if (code === null || !policy.retryableCodes.includes(code as RetryableCode)) {
    return { retry: false, delayMs: 0, attempt, code, reason: "not_retryable" };
  }
  if (priorAttempts > policy.maxRetriesPerCall) {
    return { retry: false, delayMs: 0, attempt, code, reason: "attempts_exhausted" };
  }
  return { retry: true, delayMs: policy.backoffMs, attempt, code: code ?? "unknown", reason: "transient_code" };
};

/** One deterministic retry ledger: per-call-identity attempt accounting. */
export interface RetryLedgerEntry {
  identity: string;
  attempts: number;
  failures: number;
  retried: number;
  rejected: number;
  /** True when any later attempt of the same identity succeeded. */
  recovered: boolean;
  lastOk: boolean;
  lastCode: string | null;
}

export interface RetrySummary {
  attempts: number;
  retried: number;
  rejected: number;
  recovered: number;
  byReason: Record<string, number>;
}

export class RetryLedger {
  readonly #entries = new Map<string, RetryLedgerEntry>();
  #summary: RetrySummary = { attempts: 0, retried: 0, rejected: 0, recovered: 0, byReason: {} };
  readonly #policy: RetryPolicy;

  constructor(policy: RetryPolicy = DEFAULT_RETRY_POLICY) {
    this.#policy = policy;
  }

  /**
   * Records one observed call.  `priorAttempts` is the number of times this
   * identity was already attempted; pass 0 for first attempts.  The ledger
   * only tracks repeat attempts (identity seen before).
   */
  observe(
    identity: string,
    result: Pick<ToolResult, "ok" | "error_code">,
    priorAttempts: number,
  ): RetryDecision | null {
    let entry = this.#entries.get(identity);
    if (entry === undefined) {
      entry = {
        identity,
        attempts: 0,
        failures: 0,
        retried: 0,
        rejected: 0,
        recovered: false,
        lastOk: false,
        lastCode: null,
      };
      this.#entries.set(identity, entry);
    }
    const previousCode = entry.lastCode;
    entry.attempts += 1;
    entry.lastOk = result.ok;
    entry.lastCode = result.error_code ?? null;
    if (!result.ok) entry.failures += 1;
    if (result.ok && entry.failures > 0) entry.recovered = true;

    let decision: RetryDecision | null = null;
    if (priorAttempts > 0) {
      // A repeat after a previous attempt: decide through the policy using
      // the PREVIOUS attempt's outcome (captured before this mutation).
      decision = decideRetry(this.#policy, previousCode ?? null, priorAttempts);
      if (decision.retry) entry.retried += 1;
      else entry.rejected += 1;
      this.#summary.attempts += 1;
      if (decision.retry) this.#summary.retried += 1;
      else this.#summary.rejected += 1;
      this.#summary.byReason[decision.reason] = (this.#summary.byReason[decision.reason] ?? 0) + 1;
      if (result.ok) this.#summary.recovered += 1;
    }
    return decision;
  }

  /** Prior attempts of one identity (0 when never seen). */
  priorAttempts(identity: string): number {
    const entry = this.#entries.get(identity);
    return entry?.attempts ?? 0;
  }

  entry(identity: string): RetryLedgerEntry | null {
    return this.#entries.get(identity) ?? null;
  }

  summary(): RetrySummary {
    return { ...this.#summary, byReason: { ...this.#summary.byReason } };
  }
}

/** Deterministic feedback for a blocked repeated call. */
export const renderRepeatedFailureFeedback = (code: string | null, identity: string): string =>
  `repeated call blocked: an identical call with these arguments already failed (code: ${code ?? "none"}); ` +
  `the same arguments in the same state will fail again — change the arguments or approach (call identity ${
    digestLabel(identity)
  })`;

const digestLabel = (identity: string): string => digestShort(identity);
