#pragma once
/**
 * Reliability failure classification (plan m05).
 *
 * m05 owns the reliability semantics: which failure patterns are observable
 * and how they map to deterministic classes.  The benchmark runner remains the
 * sole authority for terminal result classes (`Timeout`, `verification_failed`,
 * ...); this module classifies the RELIABILITY layer's own failures and the
 * derived per-run classification recorded in the result's `reliability`
 * summary.  The classifier is advisory for the runner and never changes
 * promotion or success decisions.
 */
#include "uos/harmony/reliability/state.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::harmony::reliability::failure {
  // "invalid_argument_loop" | "duplicate_loop" | "semantic_loop" | "stalled" | "unverified_write"
  // | "unresolved_command" | "unresolved_edit" | "false_completion" | "guard_exhausted"
  // | "transport_failed" | "no_model_output" | "tool_call_limit"
  using ReliabilityFailureClass = std::string;

  struct ReliabilityClassification {
    std::optional<ReliabilityFailureClass> failure_class;
    std::optional<std::string> detail;
    friend bool operator==(const ReliabilityClassification&, const ReliabilityClassification&) = default;
  };

  struct ClassificationInput {
    state::StructuredTaskState state;
    double invalidCallStreak;
    double loopStreak;
    double guardRejections;
    bool finalAccepted;
    /** Last run outcome when the harness aborted. */
    rt::Nullable<std::string> abortedReason{};
  };

  /** Deterministic classifier over the structured state and harness counters. */
  ReliabilityClassification classifyReliability(const ClassificationInput& input);

  // JSON members in declaration order; reading is strict (rt::TypeError naming the member).
  void tag_invoke(const json::value_from_tag&, json::value& out, const ReliabilityClassification& value);
  ReliabilityClassification tag_invoke(const json::value_to_tag<ReliabilityClassification>&, const json::value& value);
}
