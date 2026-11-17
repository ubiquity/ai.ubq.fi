// Port of tests/reliability-retry.test.ts.
#include "uos/harmony/reliability/loops.hpp"
#include "uos/harmony/reliability/retry.hpp"
#include "uos/harmony/tools/result.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::harmony::reliability::retry;
using uos::harmony::reliability::loops::callIdentity;
using uos::harmony::tools::result::ToolResult;

TEST("retry: transient codes are retried up to maxRetriesPerCall with backoff") {
  RetryPolicy policy = DEFAULT_RETRY_POLICY;
  policy.maxRetriesPerCall = 2;
  policy.backoffMs = 50;
  const RetryDecision first = decideRetry(policy, "timeout", 0);
  EXPECT(first == (RetryDecision{.retry = true, .delayMs = 50, .attempt = 1, .code = "timeout", .reason = "transient_code"}));
  const RetryDecision second = decideRetry(policy, "timeout", 1);
  EXPECT_EQ(second.retry, true);
  const RetryDecision third = decideRetry(policy, "timeout", 3);
  EXPECT_EQ(third.retry, false);
  EXPECT_EQ(third.reason, "attempts_exhausted");
}

TEST("retry: deterministic codes are never retried with identical arguments") {
  for (const std::string code : {"invalid_args", "path_escape", "write_scope", "not_found", "patch_failed", "exec_failed"}) {
    const RetryDecision decision = decideRetry(DEFAULT_RETRY_POLICY, code, 0);
    if (decision.retry != false) rt::test::report(__FILE__, __LINE__, code + " must not be retried", false);
    EXPECT_EQ(decision.reason, "not_retryable");
  }
}

TEST("retry: null code (previous call succeeded) is not retried") {
  const RetryDecision decision = decideRetry(DEFAULT_RETRY_POLICY, std::nullopt, 1);
  EXPECT_EQ(decision.retry, false);
  EXPECT_EQ(decision.reason, "not_retryable");
}

TEST("retry: the ledger counts retryable recoveries and rejected repeats") {
  RetryLedger ledger(DEFAULT_RETRY_POLICY);
  const std::string identity = callIdentity("shell.exec", json::object{{"command", "sleep 1"}});
  ledger.observe(identity, ToolResult{.ok = false, .error_code = "timeout"}, 0);
  // Second attempt with identical arguments: allowed (transient code).
  const std::optional<RetryDecision> allowed = ledger.observe(identity, ToolResult{.ok = true}, 1);
  EXPECT(allowed.has_value() && allowed->retry == true);
  const RetryLedgerEntry* entry = ledger.entry(identity);
  ASSERT(entry != nullptr);
  EXPECT_EQ(entry->recovered, true);
  const RetrySummary summary = ledger.summary();
  EXPECT(summary.attempts >= 1 && summary.retried >= 1);
}

TEST("retry: the ledger rejects a repeat after a deterministic failure") {
  RetryLedger ledger(DEFAULT_RETRY_POLICY);
  const std::string identity = callIdentity("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "x"}, {"new", "y"}});
  ledger.observe(identity, ToolResult{.ok = false, .error_code = "patch_failed"}, 0);
  const std::optional<RetryDecision> rejected = ledger.observe(identity, ToolResult{.ok = false, .error_code = "patch_failed"}, 1);
  EXPECT(rejected.has_value() && rejected->retry == false);
  ASSERT(rejected.has_value());
  EXPECT_EQ(rejected->reason, "not_retryable");
  const RetrySummary summary = ledger.summary();
  const double* notRetryable = summary.byReason.get("not_retryable");
  EXPECT(summary.rejected >= 1 && notRetryable != nullptr && *notRetryable == 1);
}

TEST("retry: repeated-failure feedback names the deterministic code") {
  const std::string text = renderRepeatedFailureFeedback("patch_failed", callIdentity("editor.apply_patch", json::object{{"path", "a"}}));
  static const rt::Regex CODE("patch_failed", "");
  static const rt::Regex ADVICE("change the arguments or approach", "");
  EXPECT(CODE.test(text));
  EXPECT(ADVICE.test(text));
}
