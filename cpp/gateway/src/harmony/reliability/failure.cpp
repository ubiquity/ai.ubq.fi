#include "uos/harmony/reliability/failure.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace uos::harmony::reliability::failure {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    /** One harness abort reason: its reliability class and detail renderer. */
    struct AbortRule {
      ReliabilityFailureClass failureClass;
      std::function<std::string(const ClassificationInput&)> detail;
    };

    /**
     * Harness abort reasons that classify deterministically.  Kept in one table so
     * the classifier reads as a lookup instead of a long branch chain; the original
     * `detail` strings are preserved verbatim.
     */
    const rt::OrderedMap<std::string, AbortRule> ABORT_RULES{
      {"false_completion", {"false_completion", [](const ClassificationInput&) { return std::string("final answer repeated without intervening action"); }}},
      {"transport_failed", {"transport_failed", [](const ClassificationInput&) { return std::string("all transport retries exhausted"); }}},
      {"no_model_output", {"no_model_output", [](const ClassificationInput&) { return std::string("model produced neither tool calls nor final content"); }}},
      {"tool_call_limit", {"tool_call_limit", [](const ClassificationInput&) { return std::string("recorded tool calls exceeded the task cap"); }}},
      {"guard_exhausted",
       {"guard_exhausted", [](const ClassificationInput& input) { return "final guard rejected " + rt::numberToString(input.guardRejections) + " attempts"; }}},
      {"invalid_argument_loop",
       {"invalid_argument_loop",
        [](const ClassificationInput& input) { return "invalid tool calls repeated (streak " + rt::numberToString(input.invalidCallStreak) + ")"; }}},
      {"turn_limit", {"stalled", [](const ClassificationInput&) { return std::string("max turns reached without completion"); }}},
    };

    /** Classifies an explicit harness abort reason, or `null` when none applies. */
    std::optional<ReliabilityClassification> classifyAbortReason(const ClassificationInput& input) {
      const rt::Nullable<std::string>& reason = input.abortedReason;
      if (reason.isNullish()) return std::nullopt;
      const AbortRule* rule = ABORT_RULES.get(*reason);
      if (rule == nullptr) return std::nullopt;
      return ReliabilityClassification{.failure_class = rule->failureClass, .detail = rule->detail(input)};
    }

    // `items.map(project).join(separator)`
    template <class Item, class Project>
    std::string join(const std::vector<Item>& items, Project project, std::string_view separator) {
      std::string joined;
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index) joined += separator;
        rt::append(joined, project(items[index]));
      }
      return joined;
    }
  }

  ReliabilityClassification classifyReliability(const ClassificationInput& input) {
    const state::StructuredTaskState& state = input.state;
    std::optional<ReliabilityClassification> aborted = classifyAbortReason(input);
    if (aborted) return std::move(*aborted);
    if (input.loopStreak >= 3 || state.semanticLoopStreak >= 3) {
      return {.failure_class = "semantic_loop", .detail = "semantic loop detected while finalizing"};
    }
    // `finals` may be empty, so the tail read is genuinely optional.
    const state::FinalObservation* lastFinal = rt::at(state.finals, -1);
    if (lastFinal != nullptr && !lastFinal->accepted && state.finalAttempts >= 2) {
      return {.failure_class = "false_completion", .detail = "final answer rejected; no evidence the task is complete"};
    }
    using PathEntry = state::StructuredTaskState::PathEntry;
    using CommandEntry = state::StructuredTaskState::CommandEntry;
    if (!state.pendingVerification.empty()) {
      return {
        .failure_class = "unverified_write",
        .detail = "unverified writes at end of run: " + join(state.pendingVerification, [](const PathEntry& p) { return p.path; }, ", "),
      };
    }
    if (!state.unresolvedCommands.empty()) {
      return {
        .failure_class = "unresolved_command",
        .detail = "unresolved command failures: " + join(state.unresolvedCommands, [](const CommandEntry& c) { return c.command; }, "; "),
      };
    }
    if (!state.unresolvedEdits.empty()) {
      return {
        .failure_class = "unresolved_edit",
        .detail = "unresolved edits: " + join(state.unresolvedEdits, [](const PathEntry& e) { return e.path; }, ", "),
      };
    }
    if (state.duplicateCalls >= 3) {
      return {.failure_class = "duplicate_loop", .detail = rt::numberToString(state.duplicateCalls) + " duplicate calls recorded"};
    }
    if (state.invalidCalls >= 3) {
      return {.failure_class = "invalid_argument_loop", .detail = rt::numberToString(state.invalidCalls) + " invalid tool calls recorded"};
    }
    return {.failure_class = std::nullopt, .detail = std::nullopt};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ReliabilityClassification& value) {
    out = Writer().orNull("failure_class", value.failure_class).orNull("detail", value.detail).done();
  }

  ReliabilityClassification tag_invoke(const json::value_to_tag<ReliabilityClassification>&, const json::value& value) {
    const Reader in(value, "ReliabilityClassification");
    return {.failure_class = in.orNull<std::string>("failure_class"), .detail = in.orNull<std::string>("detail")};
  }
}
