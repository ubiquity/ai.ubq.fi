#pragma once
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
#include "uos/harmony/tools/result.hpp"
#include "uos/rt/ordered.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uos::harmony::reliability::retry {
  /** Harness-level transient codes (transport is owned by m05, not m04). */
  inline constexpr std::string_view TRANSPORT_ERROR_CODE = "transport";

  // `ToolErrorCode | typeof TRANSPORT_ERROR_CODE`
  using RetryableCode = std::string;

  struct RetryPolicy {
    /** Maximum retries of one identical call after a failure; default 1. */
    double maxRetriesPerCall;
    /** Fixed delay before each retry, milliseconds; default 100. */
    double backoffMs;
    /** Codes that allow an identical-call retry. */
    std::vector<RetryableCode> retryableCodes;
    friend bool operator==(const RetryPolicy&, const RetryPolicy&) = default;
  };

  inline const RetryPolicy DEFAULT_RETRY_POLICY{
    .maxRetriesPerCall = 1,
    .backoffMs = 100,
    .retryableCodes = {"timeout", "internal", "unavailable", "transport"},
  };

  // The union `{ retry: true; delayMs: number; attempt: number; code: string; reason: "transient_code" }
  // | { retry: false; delayMs: 0; attempt: number; code: string | null; reason: "not_retryable" | "attempts_exhausted" }`:
  // both members carry the same fields, so one struct holds either (code is set whenever retry is true).
  struct RetryDecision {
    bool retry;
    double delayMs;
    double attempt;
    std::optional<std::string> code;
    std::string reason;  // "transient_code" | "not_retryable" | "attempts_exhausted"
    friend bool operator==(const RetryDecision&, const RetryDecision&) = default;
  };

  /**
   * Decides whether one identical call may be retried.
   *
   * @param code         error code of the previous failure (null when unknown)
   * @param priorAttempts number of prior attempts of this identical call
   */
  RetryDecision decideRetry(const RetryPolicy& policy, const std::optional<std::string>& code, double priorAttempts);

  /** One deterministic retry ledger: per-call-identity attempt accounting. */
  struct RetryLedgerEntry {
    std::string identity;
    double attempts;
    double failures;
    double retried;
    double rejected;
    /** True when any later attempt of the same identity succeeded. */
    bool recovered;
    bool lastOk;
    std::optional<std::string> lastCode;
    friend bool operator==(const RetryLedgerEntry&, const RetryLedgerEntry&) = default;
  };

  struct RetrySummary {
    double attempts;
    double retried;
    double rejected;
    double recovered;
    // `Record<string, number>`: keys are the decision reasons, in first-seen order.
    rt::OrderedMap<std::string, double> byReason;
  };

  class RetryLedger {
  public:
    explicit RetryLedger(RetryPolicy policy = DEFAULT_RETRY_POLICY);

    /**
     * Records one observed call.  `priorAttempts` is the number of times this
     * identity was already attempted; pass 0 for first attempts.  The ledger
     * only tracks repeat attempts (identity seen before).
     */
    // `result: Pick<ToolResult, "ok" | "error_code">`: only those two members are read.
    std::optional<RetryDecision> observe(const std::string& identity, const tools::result::ToolResult& result, double priorAttempts);

    /** Prior attempts of one identity (0 when never seen). */
    double priorAttempts(const std::string& identity) const;

    // The ledger's own entry (a live view, like the JS object reference), or nullptr for null.
    const RetryLedgerEntry* entry(const std::string& identity) const;

    RetrySummary summary() const;

  private:
    std::unordered_map<std::string, RetryLedgerEntry> entries_{};
    RetrySummary summary_{.attempts = 0, .retried = 0, .rejected = 0, .recovered = 0, .byReason = {}};
    RetryPolicy policy_;
  };

  /** Deterministic feedback for a blocked repeated call. */
  // `code: string | null`
  std::string renderRepeatedFailureFeedback(const std::optional<std::string>& code, const std::string& identity);
}
