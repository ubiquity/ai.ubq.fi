// Differential cases for src/harmony/reliability/retry.ts beyond tests/reliability-retry.test.ts:
// fractional, negative, NaN and infinite attempt counts, empty and foreign codes, custom policies,
// exact feedback text and the ledger's live entry objects. Expected values were recorded from the
// TypeScript module under Bun 1.4.0 (tests/reliability_retry_differential_vectors.inc).
#include "uos/harmony/reliability/loops.hpp"
#include "uos/harmony/reliability/retry.hpp"
#include "uos/harmony/tools/result.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "reliability_retry_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::harmony::reliability::retry;
using uos::harmony::reliability::loops::callIdentity;
using uos::harmony::tools::result::ToolResult;

namespace {
  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  json::value decisionJson(const RetryDecision& decision) {
    json::object object;
    object["retry"] = decision.retry;
    object["delayMs"] = rt::makeNumber(decision.delayMs);
    object["attempt"] = rt::makeNumber(decision.attempt);
    object["code"] = orNull(decision.code);
    object["reason"] = decision.reason;
    return object;
  }

  json::value entryJson(const RetryLedgerEntry* entry) {
    if (entry == nullptr) return nullptr;
    json::object object;
    object["identity"] = entry->identity;
    object["attempts"] = rt::makeNumber(entry->attempts);
    object["failures"] = rt::makeNumber(entry->failures);
    object["retried"] = rt::makeNumber(entry->retried);
    object["rejected"] = rt::makeNumber(entry->rejected);
    object["recovered"] = entry->recovered;
    object["lastOk"] = entry->lastOk;
    object["lastCode"] = orNull(entry->lastCode);
    return object;
  }

  json::value summaryJson(const RetrySummary& summary) {
    json::object byReason;
    for (const auto& [reason, count] : summary.byReason) byReason[reason] = rt::makeNumber(count);
    json::object object;
    object["attempts"] = rt::makeNumber(summary.attempts);
    object["retried"] = rt::makeNumber(summary.retried);
    object["rejected"] = rt::makeNumber(summary.rejected);
    object["recovered"] = rt::makeNumber(summary.recovered);
    object["byReason"] = std::move(byReason);
    return object;
  }

  void expectSameJson(const std::string& label, const json::value& actual, const json::value& recorded) {
    const std::string mine = rt::jsonStringify(actual);
    const std::string theirs = rt::jsonStringify(recorded);
    if (mine != theirs) EXPECT_EQ(label + " " + mine, label + " " + theirs);
  }
}

TEST("differential: decideRetry over policies, codes and attempt counts") {
  const json::value recorded = rt::jsonParse(kRetryVectorsJson);
  RetryPolicy doubled = DEFAULT_RETRY_POLICY;
  doubled.maxRetriesPerCall = 2;
  doubled.backoffMs = 50;
  const std::vector<RetryPolicy> policies = {
    DEFAULT_RETRY_POLICY,
    doubled,
    RetryPolicy{.maxRetriesPerCall = 0, .backoffMs = 0, .retryableCodes = {}},
    RetryPolicy{.maxRetriesPerCall = 1.5, .backoffMs = -5, .retryableCodes = {"patch_failed", "timeout"}},
  };
  const std::vector<std::optional<std::string>> codes = {std::nullopt, "timeout", "internal", "unavailable", "transport", "invalid_args", "", "TIMEOUT", "patch_failed"};
  const std::vector<double> priors = {0, 1, 1.5, 2, 3, -1, rt::Number::NaN, rt::Number::POSITIVE_INFINITY};
  const json::array& decisions = *rt::getArray(recorded, "decisions");
  std::size_t index = 0;
  for (const RetryPolicy& policy : policies)
    for (const std::optional<std::string>& code : codes)
      for (const double prior : priors) {
        ASSERT(index < decisions.size());
        expectSameJson("decision" + std::to_string(index), decisionJson(decideRetry(policy, code, prior)), decisions[index]);
        index += 1;
      }
  EXPECT_EQ(index, decisions.size());
}

TEST("differential: repeated-failure feedback text") {
  const json::value recorded = rt::jsonParse(kRetryVectorsJson);
  const json::array& feedback = *rt::getArray(recorded, "feedback");
  ASSERT_EQ(feedback.size(), static_cast<std::size_t>(4));
  EXPECT_EQ(renderRepeatedFailureFeedback(std::nullopt, "x"), std::string(feedback[0].get_string()));
  EXPECT_EQ(renderRepeatedFailureFeedback("timeout", ""), std::string(feedback[1].get_string()));
  EXPECT_EQ(renderRepeatedFailureFeedback("", "\xC3\xA9\xF0\x9F\x98\x80"), std::string(feedback[2].get_string()));
  EXPECT_EQ(renderRepeatedFailureFeedback("patch_failed", callIdentity("editor.apply_patch", json::object{{"path", "a"}})), std::string(feedback[3].get_string()));
}

TEST("differential: scripted ledgers, their live entries and summaries") {
  const json::value recorded = rt::jsonParse(kRetryVectorsJson);
  using Step = std::tuple<std::string, bool, std::optional<std::string>, double>;
  const std::vector<std::vector<Step>> sequences = {
    {{"a", false, "timeout", 0}, {"a", true, std::nullopt, 1}, {"a", true, std::nullopt, 2}, {"b", false, "patch_failed", 0}, {"b", false, "patch_failed", 1}, {"a", false, "internal", 3}},
    {{"x", true, std::nullopt, 0}, {"x", true, std::nullopt, 1}, {"x", false, "timeout", 2}, {"x", false, "timeout", 3}, {"x", true, std::nullopt, 4}, {"y", false, "transport", 5}},
    {{"z", false, "unavailable", 1}, {"z", false, "exec_failed", 0}, {"z", false, "timeout", 2.5}},
  };
  const RetryPolicy custom{.maxRetriesPerCall = 1.5, .backoffMs = -5, .retryableCodes = {"patch_failed", "timeout"}};
  const json::array& ledgers = *rt::getArray(recorded, "ledgers");
  ASSERT_EQ(ledgers.size(), sequences.size());
  for (std::size_t index = 0; index < sequences.size(); ++index) {
    RetryLedger ledger(index == 2 ? custom : DEFAULT_RETRY_POLICY);
    struct Seen {
      std::optional<RetryDecision> decision;
      const RetryLedgerEntry* entry;
      double prior;
      const RetryLedgerEntry* missing;
      double missingPrior;
    };
    std::vector<Seen> seen;
    for (const auto& [identity, ok, code, prior] : sequences[index]) {
      const ToolResult result{.ok = ok, .error_code = code};
      std::optional<RetryDecision> decision = ledger.observe(identity, result, prior);
      seen.push_back(Seen{decision, ledger.entry(identity), ledger.priorAttempts(identity), ledger.entry("missing"), ledger.priorAttempts("missing")});
    }
    // JSON.stringify runs after the last step, so every entry prints the ledger's final object.
    json::array steps;
    for (const Seen& step : seen) {
      json::object object;
      object["decision"] = step.decision ? decisionJson(*step.decision) : json::value(nullptr);
      object["entry"] = entryJson(step.entry);
      object["prior"] = rt::makeNumber(step.prior);
      object["missing"] = entryJson(step.missing);
      object["missingPrior"] = rt::makeNumber(step.missingPrior);
      steps.push_back(std::move(object));
    }
    json::object actual;
    actual["steps"] = std::move(steps);
    actual["summary"] = summaryJson(ledger.summary());
    expectSameJson("ledger" + std::to_string(index), actual, ledgers[index]);
  }
}

TEST("differential: the summary is a copy the ledger does not share") {
  RetryLedger ledger;
  ledger.observe("a", ToolResult{.ok = false, .error_code = "timeout"}, 0);
  ledger.observe("a", ToolResult{.ok = true}, 1);
  RetrySummary copy = ledger.summary();
  copy.attempts = 99;
  copy.byReason.set("transient_code", 99);
  copy.byReason.set("extra", 1);
  const RetrySummary fresh = ledger.summary();
  EXPECT_EQ(fresh.attempts, 1.0);
  EXPECT_EQ(*fresh.byReason.get("transient_code"), 1.0);
  EXPECT(fresh.byReason.get("extra") == nullptr);
}
