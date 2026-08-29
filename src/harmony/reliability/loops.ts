/**
 * Duplicate and semantic-loop detection (plan m05).
 *
 * The detector is a pure, deterministic state machine fed one tool call at a
 * time.  It answers two questions:
 *
 * 1. **Duplicate** — is this call identical to the immediately previous
 *    attempted call, and did that call succeed?  (Retries after *failures*
 *    are recovery, not duplication — m02 metric semantics — so the detector
 *    reports the raw adjacency fact and lets the retry policy decide whether
 *    re-execution is allowed.)
 * 2. **Semantic loop** — does the (tool, arguments, effect) signature recur
 *    within a bounded window, either as a repeating call pattern or as three
 *    repetitions of one effect (the same action producing the same result,
 *    i.e. no new information and no state change)?
 *
 * "Effect" is the deterministic result identity: `ok` plus the digest of the
 * output for successes, or the error code plus the digest of the error text
 * for failures.  Guard-rejected calls (duplicate_call / repeated_failure)
 * therefore carry a stable effect signature too, so repeated rejections are
 * themselves detected as a loop.
 *
 * This module performs no I/O.
 */

import { digestShort } from "./hash.ts";

/** Structural result view accepted by the rule engines (m04 envelope or recorded). */
export type ResultLike = Readonly<{
  ok: boolean;
  error_code?: string | null;
  error?: string | null;
  output?: string | null;
}>;

/** Sorts object keys recursively so argument canonical form is order-independent. */
export const canonicalize = (value: unknown): unknown => {
  if (Array.isArray(value)) return value.map(canonicalize);
  if (typeof value === "object" && value !== null) {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(value as Record<string, unknown>).sort()) {
      out[key] = canonicalize((value as Record<string, unknown>)[key]);
    }
    return out;
  }
  return value;
};

/** Deterministic canonical JSON form of tool arguments. */
export const canonicalArgs = (args: Record<string, unknown>): string => JSON.stringify(canonicalize(args));

/** Stable identity of one (tool, arguments) pair. */
export const callIdentity = (tool: string, args: Record<string, unknown>): string =>
  `${tool}\u0000${canonicalArgs(args)}`;

/** Digest of the model-visible effect of one result (deterministic). */
export const effectDigest = (result: ResultLike): string => {
  if (result.ok) return `ok:${digestShort(result.output ?? "")}`;
  return `fail:${result.error_code ?? "none"}:${digestShort(result.error ?? "")}`;
};

/** Full effect signature of a call + result. */
export const effectSignature = (tool: string, args: Record<string, unknown>, result: ResultLike): string =>
  `${callIdentity(tool, args)}\u0000${effectDigest(result)}`;

export type DuplicateFlag = "exact_adjacent" | "repeat_after_success";

export interface LoopFlags {
  /** Identity duplication detected (never qualifies a retry-after-failure). */
  duplicate: DuplicateFlag | null;
  /** True when this call closes a semantic loop within the window. */
  semanticLoop: boolean;
  loopKind: "pattern_recurrence" | "effect_repeat" | null;
  /** Consecutive calls flagged as semantic loops (after this call). */
  streak: number;
}

export interface LoopDetectorOptions {
  /** Rolling window of remembered call signatures; default 8. */
  window?: number;
  /** Pattern length compared for sequence recurrence; default 4. */
  patternLength?: number;
  /** Repetitions of one effect signature that close a loop; default 3. */
  effectRepeatThreshold?: number;
}

export const DEFAULT_LOOP_WINDOW = 8;
export const DEFAULT_LOOP_PATTERN_LENGTH = 4;
export const DEFAULT_EFFECT_REPEAT_THRESHOLD = 3;

/** Deterministic duplicate + semantic-loop detector over one call stream. */
export class LoopDetector {
  readonly #window: number;
  readonly #patternLength: number;
  readonly #effectThreshold: number;
  readonly #identities: (readonly [string, string])[] = []; // [call identity, effect signature]
  readonly #effects: Record<string, number> = {};
  #last: { identity: string; ok: boolean } | null = null;
  #streak = 0;
  #flags: Record<string, LoopFlags> = {};

  constructor(opts: LoopDetectorOptions = {}) {
    this.#window = opts.window ?? DEFAULT_LOOP_WINDOW;
    this.#patternLength = opts.patternLength ?? DEFAULT_LOOP_PATTERN_LENGTH;
    this.#effectThreshold = opts.effectRepeatThreshold ?? DEFAULT_EFFECT_REPEAT_THRESHOLD;
  }

  /**
   * Duplicate check BEFORE execution.  Returns `exact_adjacent` when the call
   * is identical to the immediately previous attempted call, and
   * `repeat_after_success` when that previous call also succeeded.  Should be
   * called exactly once per call, before {@link observe}.
   */
  checkDuplicate(tool: string, args: Record<string, unknown>): DuplicateFlag | null {
    const identity = callIdentity(tool, args);
    if (this.#last === null || this.#last.identity !== identity) return null;
    return this.#last.ok ? "repeat_after_success" : "exact_adjacent";
  }

  /**
   * Records one executed/guarded call with its result and returns the updated
   * loop flags.  `identityFailed` marks a call that was NOT executed because
   * of a guard (duplicate/repeated-failure rejection): its effect signature
   * is still recorded so repeated rejections close a loop.
   */
  observe(
    tool: string,
    args: Record<string, unknown>,
    result: ResultLike,
  ): LoopFlags {
    const identity = callIdentity(tool, args);
    const effect = effectSignature(tool, args, result);
    this.#identities.push([identity, effect]);
    while (this.#identities.length > this.#window) {
      const evicted = this.#identities.shift()!;
      this.#effects[evicted[1]] = Math.max(0, (this.#effects[evicted[1]] ?? 1) - 1);
    }
    this.#effects[effect] = (this.#effects[effect] ?? 0) + 1;

    // Pattern recurrence: the last `patternLength` calls repeat the
    // preceding `patternLength` calls exactly.
    let patternLoop = false;
    if (this.#identities.length >= 2 * this.#patternLength) {
      const tail = this.#identities.slice(-this.#patternLength).map((e) => e[0]);
      const prev = this.#identities.slice(-2 * this.#patternLength, -this.#patternLength).map((e) => e[0]);
      patternLoop = tail.every((id, i) => id === prev[i]);
    }
    // Effect repetition: the same effect signature repeats within the window.
    const effectLoop = (this.#effects[effect] ?? 0) >= this.#effectThreshold;

    const semanticLoop = patternLoop || effectLoop;
    this.#streak = semanticLoop ? this.#streak + 1 : 0;
    const flags: LoopFlags = {
      duplicate: this.#last !== null && this.#last.identity === identity
        ? (this.#last.ok ? "repeat_after_success" : "exact_adjacent")
        : null,
      semanticLoop,
      loopKind: patternLoop ? "pattern_recurrence" : effectLoop ? "effect_repeat" : null,
      streak: this.#streak,
    };
    this.#last = { identity, ok: result.ok };
    this.#flags[identity] = flags;
    return flags;
  }

  /** Flags for the most recent observation of a call identity (or null). */
  flagsFor(identity: string): LoopFlags | null {
    return this.#flags[identity] ?? null;
  }

  /** Resets adjacency/streak state (does not clear remembered content). */
  reset(): void {
    this.#last = null;
    this.#streak = 0;
  }
}

/** Renders a deterministic one-line loop rejection message. */
export const renderLoopFeedback = (flags: LoopFlags): string => {
  if (!flags.semanticLoop) return "";
  const kind = flags.loopKind === "pattern_recurrence"
    ? "the same call sequence keeps repeating"
    : "the same action produces the same result";
  return `semantic loop detected (${kind}); take a materially different action or verify the result — repeating this action will not change the outcome`;
};
