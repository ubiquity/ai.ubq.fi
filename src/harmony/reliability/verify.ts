/**
 * Verification requirements and false-completion prevention (plan m05).
 *
 * The reliability layer treats two claims as unproven until evidence exists:
 *
 * - **Edits**: every successful `editor.apply_patch` creates a *pending
 *   verification* for its path.  The requirement is satisfied by a
 *   subsequent `filesystem.read` of the same path whose content includes the
 *   written marker, or by a successful `shell.exec` that names the path, or
 *   by the task's declared verification command (exact match, because the
 *   benchmark runner re-runs the same command as the authoritative gate).
 * - **Recovery**: a failed `shell.exec` or failed `editor.apply_patch` must
 *   be followed by evidence before the run may finish — a later successful
 *   command (any), a successful write of the same path, or an exec that
 *   mentions the path.  A final answer while such evidence is missing is a
 *   *false completion* and is rejected deterministically.
 *
 * {@link guardFinal} is the single deterministic point of truth for final
 * acceptance; the harness (and the benchmark derivation layer) both use it.
 * This module performs no I/O and never executes tools.
 */

import { canonicalArgs, type ResultLike } from "./loops.ts";

export type VerificationKind = "read" | "shell";

/**
 * Closed set of guard error codes the reliability harness uses in tool result
 * envelopes for calls it refuses to execute.  These are m05-owned (m04's
 * {@link ToolResult.error_code} set covers execution failures); metrics and
 * state derivation treat them as guards, never as real tool errors.
 */
export const GUARD_ERROR_CODES = ["duplicate_call", "repeated_failure"] as const;

export type GuardErrorCode = (typeof GUARD_ERROR_CODES)[number];

export interface VerificationPolicy {
  /** Reject finals while any write is unverified. Default true. */
  requireVerificationBeforeFinal: boolean;
  /** Reject finals while command/edit failures are unresolved. Default true. */
  requireRecoveryBeforeFinal: boolean;
  /** Reject finals while a semantic loop is active. Default true. */
  rejectFinalDuringLoop: boolean;
  /** Reject finals before any task.update_plan when writes exist. Default false. */
  requirePlanBeforeWrites: boolean;
  /** Hard cap of final attempts before the harness aborts. Default 3. */
  maxFinalAttempts: number;
  /** Identical finals with no intervening action allowed before abort. Default 2. */
  maxRepeatedFinals: number;
  /** Task-declared verification command (exact match satisfies all writes). */
  verificationCommand: string | null;
}

export const DEFAULT_VERIFICATION_POLICY: VerificationPolicy = {
  requireVerificationBeforeFinal: true,
  requireRecoveryBeforeFinal: true,
  rejectFinalDuringLoop: true,
  requirePlanBeforeWrites: false,
  maxFinalAttempts: 3,
  maxRepeatedFinals: 2,
  verificationCommand: null,
};

export interface PendingVerification {
  path: string;
  /** Written marker (the patch `new` value) the read must contain. */
  marker: string;
  add: boolean;
  since: number;
}

export interface UnresolvedCommand {
  command: string;
  code: string;
  since: number;
}

export interface UnresolvedEdit {
  path: string;
  since: number;
}

export interface VerificationResolution {
  kind: VerificationKind;
  /** Paths whose pending verification this call satisfied. */
  paths: readonly string[];
}

export class VerificationTracker {
  readonly policy: VerificationPolicy;
  readonly #pending = new Map<string, PendingVerification>();
  readonly #unresolvedCommands = new Map<string, UnresolvedCommand>();
  readonly #unresolvedEdits = new Map<string, UnresolvedEdit>();
  #required = 0;
  #satisfied = 0;

  constructor(policy: VerificationPolicy = DEFAULT_VERIFICATION_POLICY) {
    this.policy = policy;
  }

  /**
   * Observes one executed call and updates requirements.  Returns the
   * resolution when this call satisfied a pending verification.
   */
  observe(tool: string, args: Record<string, unknown>, result: ResultLike): VerificationResolution | null {
    const path = typeof args.path === "string" ? args.path : null;
    if (tool === "task.update_plan" && result.ok) {
      // A new plan is deterministic abandonment evidence: unsolved
      // command/edit failures of the previous approach no longer block.
      this.#unresolvedCommands.clear();
      this.#unresolvedEdits.clear();
      return null;
    }
    if (tool === "editor.apply_patch") {
      if (result.ok && path !== null) {
        const marker = typeof args.new === "string" ? args.new : "";
        this.#required += 1;
        this.#pending.set(path, { path, marker, add: args.add === true, since: Date.now() });
        this.#unresolvedEdits.delete(path);
        return null;
      }
      if (!result.ok && path !== null) {
        this.#unresolvedEdits.set(path, { path, since: Date.now() });
      }
      return null;
    }
    if (tool === "filesystem.read" && result.ok && path !== null) {
      // Reading a path whose edit failed is deterministic review evidence:
      // the model inspected the actual state before moving on.
      this.#unresolvedEdits.delete(path);
      const pending = this.#pending.get(path);
      if (pending !== undefined) {
        const output = result.output ?? "";
        if (pending.marker === "" || output.includes(pending.marker)) {
          this.#pending.delete(path);
          this.#satisfied += 1;
          return { kind: "read", paths: [path] };
        }
      }
      return null;
    }
    if (tool === "shell.exec") {
      const command = typeof args.command === "string" ? args.command : "";
      if (result.ok) {
        // Any successful command is recovery evidence for earlier failures.
        this.#unresolvedCommands.clear();
        const declared = this.policy.verificationCommand?.trim();
        if (declared !== undefined && declared !== null && declared !== "" && command.trim() === declared) {
          const paths = [...this.#pending.keys()];
          this.#pending.clear();
          this.#satisfied += paths.length;
          return paths.length === 0 ? null : { kind: "shell", paths };
        }
        for (const pendingPath of [...this.#pending.keys()]) {
          if (command.includes(pendingPath)) {
            this.#pending.delete(pendingPath);
            this.#satisfied += 1;
            return { kind: "shell", paths: [pendingPath] };
          }
        }
        return null;
      }
      if (command !== "") {
        // Replace the previous failure record for the same command.
        this.#unresolvedCommands.set(command, { command, code: result.error_code ?? "failure", since: Date.now() });
      }
      return null;
    }
    return null;
  }

  pending(): readonly PendingVerification[] {
    return [...this.#pending.values()];
  }

  unresolvedCommands(): readonly UnresolvedCommand[] {
    return [...this.#unresolvedCommands.values()];
  }

  unresolvedEdits(): readonly UnresolvedEdit[] {
    return [...this.#unresolvedEdits.values()];
  }

  allVerified(): boolean {
    return this.#pending.size === 0;
  }

  totals(): { required: number; satisfied: number } {
    return { required: this.#required, satisfied: this.#satisfied };
  }

  /** Deterministic JSON snapshot for comparisons. */
  snapshot(): Record<string, unknown> {
    return {
      pending: [...this.#pending.values()].sort((a, b) => a.path.localeCompare(b.path)),
      unresolvedCommands: [...this.#unresolvedCommands.values()].sort((a, b) => a.command.localeCompare(b.command)),
      unresolvedEdits: [...this.#unresolvedEdits.values()].sort((a, b) => a.path.localeCompare(b.path)),
      totals: this.totals(),
    };
  }
}

export type FinalRequirementKind =
  | "unverified_write"
  | "unresolved_command"
  | "unresolved_edit"
  | "active_loop"
  | "false_completion"
  | "plan_required";

export interface FinalRequirement {
  kind: FinalRequirementKind;
  message: string;
  path?: string;
  command?: string;
}

export interface FinalAttempt {
  content: string;
  rejected: boolean;
  /** Sequence of the last successful tool call before this final (0 = none). */
  lastActionSeq: number;
  /** How many times this exact content was attempted without intervening action. */
  repetitions: number;
}

export interface FinalGuardInput {
  finalContent: string;
  /** Sequence of the last successful tool call before this final (0 = none). */
  lastActionSeq: number;
  previousFinals: readonly FinalAttempt[];
  semanticLoopStreak: number;
  planUpdated: boolean;
  writes: number;
  tracker: VerificationTracker;
  policy: VerificationPolicy;
}

export interface FinalGuardDecision {
  allowed: boolean;
  requirements: readonly FinalRequirement[];
  /** True when the same final content was already rejected with no action between. */
  falseCompletion: boolean;
  /** Updated attempt bookkeeping for the harness. */
  attempt: FinalAttempt;
}

/**
 * Deterministic final-answer gate.  Returns `allowed: false` with every
 * blocking requirement when the claim is not yet supported by evidence.
 */
export function guardFinal(input: FinalGuardInput): FinalGuardDecision {
  const policy = input.policy;
  const requirements: FinalRequirement[] = [];
  const prior = [...input.previousFinals].reverse().find((f) => f.content === input.finalContent);
  const repetitions = prior === undefined ? 0 : prior.repetitions + 1;
  const falseCompletion = prior !== undefined && prior.lastActionSeq === input.lastActionSeq;

  if (policy.requireVerificationBeforeFinal) {
    for (const pending of input.tracker.pending()) {
      requirements.push({
        kind: "unverified_write",
        message: `unverified write: ${pending.path} — verify it before answering`,
        path: pending.path,
      });
    }
  }
  if (policy.requireRecoveryBeforeFinal) {
    for (const unresolved of input.tracker.unresolvedCommands()) {
      requirements.push({
        kind: "unresolved_command",
        message:
          `unresolved command failure: ${unresolved.command} — recover with a successful command before answering`,
        command: unresolved.command,
      });
    }
    for (const unresolved of input.tracker.unresolvedEdits()) {
      requirements.push({
        kind: "unresolved_edit",
        message: `unresolved edit: ${unresolved.path} — apply the change or verify the path before answering`,
        path: unresolved.path,
      });
    }
  }
  if (policy.rejectFinalDuringLoop && input.semanticLoopStreak >= 3) {
    requirements.push({
      kind: "active_loop",
      message: "active semantic loop — take a materially different action before answering",
    });
  }
  if (falseCompletion && repetitions >= 1) {
    requirements.push({
      kind: "false_completion",
      message:
        "final answer repeated without any intervening action — the previous claim was rejected; perform a verification or a different action first",
    });
  }
  if (
    policy.requirePlanBeforeWrites && input.writes > 0 && !input.planUpdated
  ) {
    requirements.push({
      kind: "plan_required",
      message: "writes happened without a plan — call task.update_plan before answering",
    });
  }

  const attempt: FinalAttempt = {
    content: input.finalContent,
    rejected: requirements.length > 0,
    lastActionSeq: input.lastActionSeq,
    repetitions,
  };
  return { allowed: requirements.length === 0, requirements, falseCompletion, attempt };
}

/** One deterministic line listing every unmet requirement. */
export const renderGuardRequirements = (requirements: readonly FinalRequirement[]): string =>
  requirements.length === 0 ? "" : requirements.map((r) => `[${r.kind}] ${r.message}`).join("; ");

/** Stable prefix of every harness guard message handed back to the model. */
export const GUARD_PREFIX = "[guard] final answer rejected";

/** Deterministic canonical label of a call, for guard messages. */
export const callLabel = (tool: string, args: Record<string, unknown>): string => `${tool}(${canonicalArgs(args)})`;
