/**
 * m05 reliability derivation over benchmark trajectory events.
 *
 * The runner is adapter-agnostic, so the reliability summary of every result
 * is derived deterministically from the recorded event stream — never from
 * model text or adapter prose.  This module maps m02 trajectory events into
 * the m05 observation stream and replays the same rule engines the live
 * harness uses (loop detector, verification tracker, retry ledger), then
 * attaches the structured state contract and the advisory reliability class.
 *
 * Watching rules (m02 metrics semantics are preserved):
 * - retries after a FAILED identical call are recovery, not duplication;
 *   the loop detector reports adjacency, the retry policy decides allowance;
 * - guard envelopes (`duplicate_call` / `repeated_failure`) are never
 *   counted as tool errors or verification evidence.
 */

import { classifyReliability } from "../src/harmony/reliability/failure.ts";
import { callIdentity } from "../src/harmony/reliability/loops.ts";
import { decideRetry, DEFAULT_RETRY_POLICY, RetryLedger } from "../src/harmony/reliability/retry.ts";
import {
  deriveStateWithMeta,
  type FinalObservation,
  replayMeta,
  stateContract,
  type ToolObservation,
} from "../src/harmony/reliability/state.ts";
import {
  DEFAULT_VERIFICATION_POLICY,
  type VerificationPolicy,
  VerificationTracker,
} from "../src/harmony/reliability/verify.ts";
import type { ReliabilitySummary, TrajectoryEvent } from "./schemas.ts";

export interface ReliabilityDerivation {
  state_contract: string;
  summary: ReliabilitySummary;
}

type ToolCallEvent = Extract<TrajectoryEvent, { type: "tool_call" }>;
type ToolResultEvent = Extract<TrajectoryEvent, { type: "tool_result" }>;

const isGuardCode = (code: string | null | undefined): boolean =>
  code === "duplicate_call" || code === "repeated_failure";

/** Maps recorded events into the m05 observation stream (deterministic). */
export function observationsFromEvents(events: readonly TrajectoryEvent[]): ToolObservation[] {
  const results = new Map<string, ToolResultEvent>();
  for (const event of events) {
    if (event.type === "tool_result") results.set(event.id, event);
  }
  const observations: ToolObservation[] = [];
  let seq = 0;
  for (const event of events) {
    if (event.type !== "tool_call") continue;
    seq += 1;
    const resultEvent = results.get(event.id);
    observations.push({
      seq,
      tool: event.tool,
      args: event.arguments,
      valid: event.valid,
      result: resultEvent === undefined ? null : {
        ok: resultEvent.ok,
        error_code: resultEvent.error_code ?? null,
        error: resultEvent.error ?? null,
        output: resultEvent.output ?? null,
      },
    });
  }
  return observations;
}

/** Maps recorded final answers (content without tool calls) deterministically. */
export function finalsFromEvents(events: readonly TrajectoryEvent[]): FinalObservation[] {
  const finals: FinalObservation[] = [];
  const calls = events.filter((e): e is ToolCallEvent => e.type === "tool_call");
  const lastCallIndex = calls.length === 0 ? -1 : events.indexOf(calls[calls.length - 1]);
  let seq = 0;
  for (const event of events) {
    if (event.type !== "model_response") continue;
    if (event.tool_calls !== undefined && event.tool_calls.length > 0) continue;
    if (event.content === undefined || event.content === null || event.content.length === 0) continue;
    seq += 1;
    const index = events.indexOf(event);
    const accepted = index > lastCallIndex &&
      !events.slice(index + 1).some((e) => e.type === "guard" || e.type === "tool_call");
    finals.push({ content: event.content, accepted, seq });
  }
  return finals;
}

/** Trailing consecutive invalid calls (0 when the run ends valid). */
export function trailingInvalidStreak(observations: readonly ToolObservation[]): number {
  let streak = 0;
  for (let i = observations.length - 1; i >= 0; i--) {
    if (observations[i].valid) break;
    streak += 1;
  }
  return streak;
}

/** Replays the retry ledger over events (same policy as the live harness). */
export function retrySummaryFromEvents(events: readonly TrajectoryEvent[]): ReliabilitySummary["retries"] {
  const ledger = new RetryLedger(DEFAULT_RETRY_POLICY);
  const calls = events.filter((e): e is ToolCallEvent => e.type === "tool_call");
  const results = new Map(
    events.filter((e): e is ToolResultEvent => e.type === "tool_result").map((e) => [e.id, e]),
  );
  let attempts = 0;
  let allowed = 0;
  let rejected = 0;
  for (const [index, event] of calls.entries()) {
    const identity = callIdentity(event.tool, event.arguments);
    const priorAttempts = ledger.priorAttempts(identity);
    if (priorAttempts > 0) {
      attempts += 1;
      const previous = [...calls.slice(0, index)].reverse().find((c) => callIdentity(c.tool, c.arguments) === identity);
      const previousResult = previous === undefined ? undefined : results.get(previous.id);
      const previousCode = previousResult === undefined || previousResult.ok ? null : previousResult.error_code ?? null;
      const decision = decideRetry(DEFAULT_RETRY_POLICY, previousCode, priorAttempts);
      if (decision.retry) allowed += 1;
      else rejected += 1;
    }
    const result = results.get(event.id);
    const observed = result === undefined ? { ok: false, error_code: "internal" as const } : {
      ok: result.ok,
      error_code: result.error_code as import("../src/harmony/tools/result.ts").ToolErrorCode | undefined,
    };
    ledger.observe(identity, observed, priorAttempts);
  }
  return { attempts, allowed, rejected };
}

/**
 * Derives the m05 reliability evidence for one recorded run.  `events` must
 * include the `run` event and every tool_call/tool_result pair, exactly like
 * the trajectory files the runner persists.
 */
export function deriveReliability(
  events: readonly TrajectoryEvent[],
  opts: { verificationCommand?: string | null } = {},
): ReliabilityDerivation {
  const policy: VerificationPolicy = {
    ...DEFAULT_VERIFICATION_POLICY,
    verificationCommand: opts.verificationCommand ?? null,
  };
  const observations = observationsFromEvents(events);
  const finals = finalsFromEvents(events);
  const modelCalls = events.filter((e) => e.type === "model_request").length;
  const meta = replayMeta(observations, policy);
  const state = deriveStateWithMeta({ observations, finals, modelCalls }, meta);

  const guards = events.filter((e): e is Extract<TrajectoryEvent, { type: "guard" }> => e.type === "guard");
  const guardRejections = guards.filter((g) => g.attempt > 0).length;
  const falseCompletions = guards.filter((g) => g.kind === "false_completion").length;
  const invalidStreak = trailingInvalidStreak(observations);

  const tracker = new VerificationTracker(policy);
  for (const obs of observations) {
    const result = obs.result;
    if (!obs.valid || result === null || result === undefined) continue;
    if (isGuardCode(result.error_code)) continue;
    tracker.observe(obs.tool, obs.args, result);
  }

  const classification = classifyReliability({
    state,
    invalidCallStreak: invalidStreak,
    loopStreak: state.semanticLoopStreak,
    guardRejections,
    finalAccepted: finals.some((f) => f.accepted),
    abortedReason: null,
  });

  const summary: ReliabilitySummary = {
    phase: state.phase,
    final_accepted: finals.some((f) => f.accepted),
    guard_rejections: guardRejections,
    false_completions: falseCompletions,
    invalid_call_streak: invalidStreak,
    duplicate_calls: state.duplicateCalls,
    semantic_loops: state.semanticLoops,
    retries: retrySummaryFromEvents(events),
    unverified_writes: state.pendingVerification.length,
    unresolved: { commands: state.unresolvedCommands.length, edits: state.unresolvedEdits.length },
    verification: tracker.totals(),
    failure_class: classification.failure_class,
    state_contract: stateContract(state),
  };
  return { state_contract: summary.state_contract, summary };
}
