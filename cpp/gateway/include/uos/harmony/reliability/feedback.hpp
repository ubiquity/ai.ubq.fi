#pragma once
// Port of src/harmony/reliability/feedback.ts.
/**
 * Deterministic tool-argument validation feedback (plan m05).
 *
 * m04's `schemas.ts` validates against the canonical surface but reports only
 * the first problem.  The reliability layer needs **complete, deterministic,
 * machine-readable** feedback: every issue in one pass, a stable issue code
 * per problem class, and one concise corrective hint per issue so the model
 * can fix the call without a round trip.  This module is a pure read-only
 * view over the canonical schemas:
 *
 * - {@link validateToolArgumentsDetailed} checks every present argument
 *   (unknown keys rejected, required string parameters non-empty, arrays of
 *   strings), collects ALL issues, and never mutates the input.
 * - {@link renderValidationFeedback} renders one deterministic line used by
 *   the reliability harness as the model-facing tool error text; the closed
 *   {@link ToolErrorCode} `invalid_args` stays the envelope code, so run-level
 *   bookkeeping (m02 metrics) keeps working unchanged.
 *
 * This module performs no I/O and never executes tools.
 */
#include "uos/rt/prelude.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::reliability::feedback {
  /** Stable issue classes emitted by the detailed validator. */
  // "unknown_tool" | "not_an_object" | "unexpected_argument" | "missing_required" | "wrong_type" | "non_empty"
  // | "undefined_value" | "bad_array_item"
  using ArgumentIssueCode = std::string;

  /** One argument-validation issue with a corrective hint. */
  struct ToolArgumentIssue {
    ArgumentIssueCode code;
    /** Dotted property path, e.g. `arguments.path`. */
    std::string location;
    /** Deterministic description. */
    std::string message;
    /** Short corrective hint for the model. */
    std::string hint;
    friend bool operator==(const ToolArgumentIssue&, const ToolArgumentIssue&) = default;
  };

  // `{ valid: true; ... } | { valid: false; ... }`: both arms carry the same members.
  struct DetailedValidationResult {
    bool valid;
    json::object arguments;
    std::vector<ToolArgumentIssue> issues;
    friend bool operator==(const DetailedValidationResult&, const DetailedValidationResult&) = default;
  };

  /**
   * Validates a tool call against the canonical schema and reports EVERY
   * problem deterministically.  Unknown tools produce one `unknown_tool` issue;
   * any other malformed call produces one issue per problem, sorted by a stable
   * order (unknown keys first, then required-missing, then value problems in
   * declared-property order) so output is reproducible.
   */
  // `args: unknown`: nullptr is undefined. Throws rt::TypeError "undefined is not an object
  // (evaluating 'schema.parameters.properties')" for an Object.prototype tool name with object
  // arguments, as the TS does.
  DetailedValidationResult validateToolArgumentsDetailed(std::string_view tool, const json::value& args);
  DetailedValidationResult validateToolArgumentsDetailed(std::string_view tool, const json::value* args);

  /**
   * Renders the complete feedback as ONE deterministic line.  Kept short so the
   * model-facing tool error stays within the m04 output budget while carrying
   * every issue with its code and corrective hint.
   */
  std::string renderValidationFeedback(std::string_view tool, const DetailedValidationResult& result);

  /** Short stable label for one invalid call (used in event bookkeeping). */
  std::string invalidCallLabel(const DetailedValidationResult& result);
}
