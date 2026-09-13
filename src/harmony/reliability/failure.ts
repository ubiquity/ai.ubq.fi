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

import type { StructuredTaskState } from "./state.ts";

export type ReliabilityFailureClass =
  | "invalid_argument_loop"
  | "duplicate_loop"
  | "semantic_loop"
  | "stalled"
  | "unverified_write"
  | "unresolved_command"
  | "unresolved_edit"
  | "false_completion"
  | "guard_exhausted"
  | "transport_failed"
  | "no_model_output"
  | "tool_call_limit";

export type ReliabilityClassification = {
  failure_class: ReliabilityFailureClass | null;
  detail: string | null;
};

export type ClassificationInput = {
  state: StructuredTaskState;
  invalidCallStreak: number;
  loopStreak: number;
  guardRejections: number;
  finalAccepted: boolean;
  /** Last run outcome when the harness aborted. */
  abortedReason?: string | null;
};

/** One harness abort reason: its reliability class and detail renderer. */
type AbortRule = {
  failureClass: ReliabilityFailureClass;
  detail: (input: ClassificationInput) => string;
};

/**
 * Harness abort reasons that classify deterministically.  Kept in one table so
 * the classifier reads as a lookup instead of a long branch chain; the original
 * `detail` strings are preserved verbatim.
 */
const ABORT_RULES: ReadonlyMap<string, AbortRule> = new Map<string, AbortRule>([
  ["false_completion", { failureClass: "false_completion", detail: () => "final answer repeated without intervening action" }],
  ["transport_failed", { failureClass: "transport_failed", detail: () => "all transport retries exhausted" }],
  ["no_model_output", { failureClass: "no_model_output", detail: () => "model produced neither tool calls nor final content" }],
  ["tool_call_limit", { failureClass: "tool_call_limit", detail: () => "recorded tool calls exceeded the task cap" }],
  ["guard_exhausted", { failureClass: "guard_exhausted", detail: (input) => `final guard rejected ${input.guardRejections} attempts` }],
  ["invalid_argument_loop", { failureClass: "invalid_argument_loop", detail: (input) => `invalid tool calls repeated (streak ${input.invalidCallStreak})` }],
  ["turn_limit", { failureClass: "stalled", detail: () => "max turns reached without completion" }],
]);

/** Classifies an explicit harness abort reason, or `null` when none applies. */
const classifyAbortReason = (input: ClassificationInput): ReliabilityClassification | null => {
  const reason = input.abortedReason;
  if (reason === null || reason === undefined) return null;
  const rule = ABORT_RULES.get(reason);
  if (rule === undefined) return null;
  return { failure_class: rule.failureClass, detail: rule.detail(input) };
};

/** Deterministic classifier over the structured state and harness counters. */
export function classifyReliability(input: ClassificationInput): ReliabilityClassification {
  const state = input.state;
  const aborted = classifyAbortReason(input);
  if (aborted !== null) return aborted;
  if (input.loopStreak >= 3 || state.semanticLoopStreak >= 3) {
    return { failure_class: "semantic_loop", detail: "semantic loop detected while finalizing" };
  }
  // `finals` may be empty, so the tail read is genuinely optional.
  const lastFinal = state.finals.at(-1);
  if (lastFinal !== undefined && !lastFinal.accepted && state.finalAttempts >= 2) {
    return { failure_class: "false_completion", detail: "final answer rejected; no evidence the task is complete" };
  }
  if (state.pendingVerification.length > 0) {
    return {
      failure_class: "unverified_write",
      detail: `unverified writes at end of run: ${state.pendingVerification.map((p) => p.path).join(", ")}`,
    };
  }
  if (state.unresolvedCommands.length > 0) {
    return {
      failure_class: "unresolved_command",
      detail: `unresolved command failures: ${state.unresolvedCommands.map((c) => c.command).join("; ")}`,
    };
  }
  if (state.unresolvedEdits.length > 0) {
    return {
      failure_class: "unresolved_edit",
      detail: `unresolved edits: ${state.unresolvedEdits.map((e) => e.path).join(", ")}`,
    };
  }
  if (state.duplicateCalls >= 3) {
    return { failure_class: "duplicate_loop", detail: `${state.duplicateCalls} duplicate calls recorded` };
  }
  if (state.invalidCalls >= 3) {
    return { failure_class: "invalid_argument_loop", detail: `${state.invalidCalls} invalid tool calls recorded` };
  }
  return { failure_class: null, detail: null };
}
