#include "uos/harmony/reliability/retry.hpp"

#include "uos/harmony/reliability/hash.hpp"
#include "uos/rt/json.hpp"

#include <utility>

namespace uos::harmony::reliability::retry {
  namespace {
    std::string digestLabel(const std::string& identity);
  }

  RetryDecision decideRetry(const RetryPolicy& policy, const std::optional<std::string>& code, double priorAttempts) {
    const double attempt = priorAttempts + 1;
    // A null code means the previous identical call SUCCEEDED: re-executing it
    // is a duplicate action, never a retry.
    if (!code || !rt::includes(policy.retryableCodes, *code)) {
      return RetryDecision{.retry = false, .delayMs = 0, .attempt = attempt, .code = code, .reason = "not_retryable"};
    }
    if (priorAttempts > policy.maxRetriesPerCall) {
      return RetryDecision{.retry = false, .delayMs = 0, .attempt = attempt, .code = code, .reason = "attempts_exhausted"};
    }
    return RetryDecision{.retry = true, .delayMs = policy.backoffMs, .attempt = attempt, .code = code, .reason = "transient_code"};
  }

  RetryLedger::RetryLedger(RetryPolicy policy) : policy_(std::move(policy)) {}

  std::optional<RetryDecision> RetryLedger::observe(const std::string& identity, const tools::result::ToolResult& result, double priorAttempts) {
    auto found = entries_.find(identity);
    if (found == entries_.end()) {
      RetryLedgerEntry created{
        .identity = identity,
        .attempts = 0,
        .failures = 0,
        .retried = 0,
        .rejected = 0,
        .recovered = false,
        .lastOk = false,
        .lastCode = std::nullopt,
      };
      found = entries_.emplace(identity, std::move(created)).first;
    }
    RetryLedgerEntry& entry = found->second;
    const std::optional<std::string> previousCode = entry.lastCode;
    entry.attempts += 1;
    entry.lastOk = result.ok;
    entry.lastCode = result.error_code;
    if (!result.ok) entry.failures += 1;
    if (result.ok && entry.failures > 0) entry.recovered = true;

    std::optional<RetryDecision> decision;
    if (priorAttempts > 0) {
      // A repeat after a previous attempt: decide through the policy using
      // the PREVIOUS attempt's outcome (captured before this mutation).
      decision = decideRetry(policy_, previousCode, priorAttempts);
      if (decision->retry) entry.retried += 1;
      else entry.rejected += 1;
      summary_.attempts += 1;
      if (decision->retry) summary_.retried += 1;
      else summary_.rejected += 1;
      const double* counted = summary_.byReason.get(decision->reason);
      summary_.byReason.set(decision->reason, (counted != nullptr ? *counted : 0) + 1);
      if (result.ok) summary_.recovered += 1;
    }
    return decision;
  }

  double RetryLedger::priorAttempts(const std::string& identity) const {
    const RetryLedgerEntry* found = entry(identity);
    return found != nullptr ? found->attempts : 0;
  }

  const RetryLedgerEntry* RetryLedger::entry(const std::string& identity) const {
    const auto found = entries_.find(identity);
    return found != entries_.end() ? &found->second : nullptr;
  }

  RetrySummary RetryLedger::summary() const { return summary_; }

  std::string renderRepeatedFailureFeedback(const std::optional<std::string>& code, const std::string& identity) {
    return "repeated call blocked: an identical call with these arguments already failed (code: " + code.value_or("none") +
           "); the same arguments in the same state will fail again — change the arguments or approach (call identity " + digestLabel(identity) + ")";
  }

  namespace {
    std::string digestLabel(const std::string& identity) { return hash::digestShort(identity); }
  }
}
