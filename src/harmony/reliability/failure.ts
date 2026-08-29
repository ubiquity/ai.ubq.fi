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

export const RELIABILITY_FAILURE_CLASSES: readonly ReliabilityFailureClass[] = [
  "invalid_argument_loop",
  "duplicate_loop",
  "semantic_loop",
  "stalled",
  "unverified_write",
  "unresolved_command",
  "unresolved_edit",
  "false_completion",
  "guard_exhausted",
  "transport_failed",
  "no_model_output",
  "tool_call_limit",
];

export interface ReliabilityClassification {
  failure_class: ReliabilityFailureClass | null;
  detail: string | null;
}

export interface ClassificationInput {
  state: StructuredTaskState;
  invalidCallStreak: number;
  loopStreak: number;
  guardRejections: number;
  finalAccepted: boolean;
  /** Last run outcome when the harness aborted. */
  abortedReason?: string | null;
}

/** Deterministic classifier over the structured state and harness counters. */
export function classifyReliability(input: ClassificationInput): ReliabilityClassification {
  const state = input.state;
  if (input.abortedReason !== null && input.abortedReason !== undefined) {
    if (input.abortedReason === "false_completion") {
      return { failure_class: "false_completion", detail: "final answer repeated without intervening action" };
    }
    if (input.abortedReason === "transport_failed") {
      return { failure_class: "transport_failed", detail: "all transport retries exhausted" };
    }
    if (input.abortedReason === "no_model_output") {
      return { failure_class: "no_model_output", detail: "model produced neither tool calls nor final content" };
    }
    if (input.abortedReason === "tool_call_limit") {
      return { failure_class: "tool_call_limit", detail: "recorded tool calls exceeded the task cap" };
    }
    if (input.abortedReason === "guard_exhausted") {
      return { failure_class: "guard_exhausted", detail: `final guard rejected ${input.guardRejections} attempts` };
    }
    if (input.abortedReason === "invalid_argument_loop") {
      return {
        failure_class: "invalid_argument_loop",
        detail: `invalid tool calls repeated (streak ${input.invalidCallStreak})`,
      };
    }
    if (input.abortedReason === "turn_limit") {
      return { failure_class: "stalled", detail: "max turns reached without completion" };
    }
  }
  if (input.loopStreak >= 3 || state.semanticLoopStreak >= 3) {
    return { failure_class: "semantic_loop", detail: "semantic loop detected while finalizing" };
  }
  const lastFinal = state.finals[state.finals.length - 1];
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
