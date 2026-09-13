/**
 * Structured task state and the state contract (plan m05).
 *
 * The benchmark tracks two views of a run:
 *
 * - the **full transcript** (every message, every tool result) — maximal
 *   fidelity, maximum context cost;
 * - the **structured task state** — the compact, decision-relevant projection
 *   of what the agent has done and what is still unproven.
 *
 * `StructuredTaskState` is derived deterministically from tool observations
 * by REPLAYING the same rule engines the live harness uses — the loop
 * detector (`loops.ts`) and the verification tracker (`verify.ts`) — so a
 * recorded trajectory and a live run always produce the same state.
 * {@link stateContract} renders the decision-relevant projection as canonical
 * JSON; the comparison evidence (`benchmarks/compare.ts`) proves that
 * transcript compaction preserves this contract at short/medium/large budget
 * tiers, which is exactly the m05 claim that a structured context can replace
 * a full replay without losing state.
 *
 * This module performs no I/O.
 */

import { digestShort } from "./hash.ts";
import { LoopDetector } from "./loops.ts";
import { DEFAULT_VERIFICATION_POLICY, GUARD_ERROR_CODES, type VerificationPolicy, type VerificationResolution, VerificationTracker } from "./verify.ts";

export type TaskPhase = "planning" | "exploring" | "acting" | "recovering" | "verifying" | "completing" | "stalled" | "done";

export type ToolObservation = {
  /** Monotonic attempt sequence (includes invalid and guarded calls). */
  seq: number;
  tool: string;
  args: Record<string, unknown>;
  valid: boolean;
  /** Result when the call was executed or resolved by a guard. */
  result?: {
    ok: boolean;
    error_code?: string | null;
    error?: string | null;
    output?: string | null;
  } | null;
};

export type FinalObservation = {
  content: string;
  accepted: boolean;
  seq: number;
};

export type ReliabilityRun = {
  observations: readonly ToolObservation[];
  finals: readonly FinalObservation[];
  modelCalls: number;
};

export type ReadRecord = {
  path: string;
  /** Digest of the last read content of this path. */
  digest: string;
  /** Last observation sequence touching this path. */
  seq: number;
};

export type WriteRecord = {
  path: string;
  marker: string;
  add: boolean;
  verified: boolean;
  verifiedBy: "read" | "shell" | null;
  seq: number;
};

export type StateErrorRecord = {
  code: string;
  tool: string;
  seq: number;
  /** Short deterministic fingerprint of the error text. */
  digest: string;
};

export type StructuredTaskState = {
  phase: TaskPhase;
  modelCalls: number;
  toolCalls: number;
  invalidCalls: number;
  invalidCallStreak: number;
  duplicateCalls: number;
  semanticLoops: number;
  semanticLoopStreak: number;
  plan: { items: readonly string[]; seq: number | null };
  reads: readonly ReadRecord[];
  writes: readonly WriteRecord[];
  errors: readonly StateErrorRecord[];
  pendingVerification: readonly { path: string; seq: number }[];
  unresolvedCommands: readonly { command: string; seq: number }[];
  unresolvedEdits: readonly { path: string; seq: number }[];
  /** Sequence of the last successful tool call (0 = none yet). */
  lastActionSeq: number;
  finals: readonly FinalObservation[];
  finalAttempts: number;
};

const MAX_ERRORS = 5;

export const emptyTaskState = (): StructuredTaskState => ({
  phase: "planning",
  modelCalls: 0,
  toolCalls: 0,
  invalidCalls: 0,
  invalidCallStreak: 0,
  duplicateCalls: 0,
  semanticLoops: 0,
  semanticLoopStreak: 0,
  plan: { items: [], seq: null },
  reads: [],
  writes: [],
  errors: [],
  pendingVerification: [],
  unresolvedCommands: [],
  unresolvedEdits: [],
  lastActionSeq: 0,
  finals: [],
  finalAttempts: 0,
});

export type ObservationMeta = {
  duplicate: string | null;
  semanticLoop: boolean;
  verification: VerificationResolution | null;
};

const isGuardResult = (result: ToolObservation["result"]): boolean =>
  result?.error_code !== undefined && result.error_code !== null && (GUARD_ERROR_CODES as readonly string[]).includes(result.error_code);

/** Converts a recorded observation result into the canonical envelope shape. */
export const toToolResult = (result: ToolObservation["result"]): { ok: boolean; error_code?: string; error?: string; output?: string } | null => {
  if (result === undefined || result === null) return null;
  return {
    ok: result.ok,
    error_code: result.error_code ?? undefined,
    error: result.error ?? undefined,
    output: result.output ?? undefined,
  };
};

/** Replays a deterministic rule stream over observations. */
export function replayMeta(observations: readonly ToolObservation[], policy: VerificationPolicy = DEFAULT_VERIFICATION_POLICY): readonly ObservationMeta[] {
  const detector = new LoopDetector();
  const tracker = new VerificationTracker(policy);
  const meta: ObservationMeta[] = [];
  for (const obs of observations) {
    const result = toToolResult(obs.result) ?? { ok: false, error_code: "internal", error: "missing result" };
    const isExecuted = obs.valid && !isGuardResult(obs.result);
    const flags = detector.observe(obs.tool, obs.args, result);
    const verification = isExecuted ? tracker.observe(obs.tool, obs.args, result) : null;
    meta.push({
      duplicate: flags.duplicate,
      semanticLoop: flags.semanticLoop,
      verification,
    });
  }
  return meta;
}

const phaseFor = (state: StructuredTaskState, obs: ToolObservation): TaskPhase => {
  if (state.semanticLoopStreak >= 3) return "stalled";
  if (state.finals.some((f) => f.accepted)) return "done";
  if (state.finalAttempts > 0 && state.finals.length > 0) {
    const lastFinal = state.finals[state.finals.length - 1];
    if (!lastFinal.accepted) return "completing";
  }
  if (obs.result !== undefined && obs.result !== null && !obs.result.ok && !isGuardResult(obs.result)) {
    return "recovering";
  }
  if (state.pendingVerification.length > 0 || state.unresolvedEdits.length > 0) return "verifying";
  if (state.writes.length > 0) return "acting";
  if (state.plan.seq === null && state.reads.length === 0) return "planning";
  return "exploring";
};

/**
 * Path argument of a tool call.  Values are untrusted, so a non-string `path`
 * is treated as absent instead of being stringified into a bogus record key.
 */
const stringArg = (value: unknown): string => (typeof value === "string" ? value : "");

const upsertRead = (state: StructuredTaskState, obs: ToolObservation): void => {
  const path = stringArg(obs.args.path);
  const digest = digestShort(obs.result?.output ?? "");
  state.reads = [...state.reads.filter((r) => r.path !== path), { path, digest, seq: obs.seq }].sort((a, b) => a.path.localeCompare(b.path));
};

const upsertWrite = (state: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta): void => {
  const path = stringArg(obs.args.path);
  const marker = typeof obs.args.new === "string" ? obs.args.new : "";
  const rest = state.writes.filter((w) => w.path !== path);
  const verified = meta.verification !== null;
  state.writes = [
    ...rest,
    {
      path,
      marker,
      add: obs.args.add === true,
      verified,
      verifiedBy: meta.verification?.kind ?? null,
      seq: obs.seq,
    },
  ].sort((a, b) => a.path.localeCompare(b.path));
};

const recordError = (state: StructuredTaskState, obs: ToolObservation): void => {
  const result = obs.result;
  if (result === undefined || result === null || result.ok || isGuardResult(result)) return;
  state.errors = [
    ...state.errors,
    {
      code: result.error_code ?? "failure",
      tool: obs.tool,
      seq: obs.seq,
      digest: digestShort(result.error ?? ""),
    },
  ].slice(-MAX_ERRORS);
};

/** Applies one verification resolution to the pending set and the write records. */
const applyVerificationToWrites = (next: StructuredTaskState, verification: VerificationResolution): void => {
  const paths = new Set(verification.paths);
  next.pendingVerification = next.pendingVerification.filter((p) => !paths.has(p.path));
  next.writes = next.writes.map((w) => (paths.has(w.path) ? { ...w, verified: true, verifiedBy: verification.kind } : w));
};

const applyPlanUpdate = (next: StructuredTaskState, obs: ToolObservation, result: ToolObservation["result"]): void => {
  if (!(result?.ok && Array.isArray(obs.args.plan))) return;
  next.plan = {
    items: (obs.args.plan as unknown[]).filter((item): item is string => typeof item === "string"),
    seq: obs.seq,
  };
  next.unresolvedCommands = [];
  next.unresolvedEdits = [];
};

const applyPatchObservation = (next: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta, result: ToolObservation["result"]): void => {
  if (result?.ok && typeof obs.args.path === "string") {
    const path = obs.args.path;
    upsertWrite(next, obs, meta);
    next.pendingVerification = [...next.pendingVerification.filter((p) => p.path !== path), { path, seq: obs.seq }];
  } else if (!isGuardResult(result) && typeof obs.args.path === "string") {
    const path = obs.args.path;
    next.unresolvedEdits = [...next.unresolvedEdits.filter((e) => e.path !== path), { path, seq: obs.seq }];
  }
};

const applyReadObservation = (next: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta, result: ToolObservation["result"]): void => {
  if (!result?.ok) return;
  upsertRead(next, obs);
  if (typeof obs.args.path === "string") {
    const readPath = obs.args.path;
    next.unresolvedEdits = next.unresolvedEdits.filter((e) => e.path !== readPath);
  }
  const verification = meta.verification;
  if (verification !== null) applyVerificationToWrites(next, verification);
};

const applyShellObservation = (next: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta, result: ToolObservation["result"]): void => {
  if (result?.ok) {
    next.unresolvedCommands = [];
    const verification = meta.verification;
    if (verification !== null) applyVerificationToWrites(next, verification);
  } else if (typeof obs.args.command === "string" && !isGuardResult(result)) {
    const command = obs.args.command;
    next.unresolvedCommands = [...next.unresolvedCommands.filter((c) => c.command !== command), { command, seq: obs.seq }];
  }
};

/** Routes one observation to the handler for its tool. */
const applyObservation = (next: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta, result: ToolObservation["result"]): void => {
  if (obs.tool === "task.update_plan") {
    applyPlanUpdate(next, obs, result);
  } else if (obs.tool === "editor.apply_patch") {
    applyPatchObservation(next, obs, meta, result);
  } else if (obs.tool === "filesystem.read") {
    applyReadObservation(next, obs, meta, result);
  } else if (obs.tool === "shell.exec") {
    applyShellObservation(next, obs, meta, result);
  }
};

/** Applies one tool observation with its replayed meta to the state. */
export function reduceToolObservation(state: StructuredTaskState, obs: ToolObservation, meta: ObservationMeta): StructuredTaskState {
  const next: StructuredTaskState = {
    ...state,
    toolCalls: state.toolCalls + 1,
    reads: state.reads,
    writes: state.writes,
    errors: state.errors,
    pendingVerification: state.pendingVerification,
    unresolvedCommands: state.unresolvedCommands,
    unresolvedEdits: state.unresolvedEdits,
    finals: state.finals,
    plan: state.plan,
  };
  if (!obs.valid) {
    next.invalidCalls += 1;
    next.invalidCallStreak = state.invalidCallStreak + 1;
  } else {
    next.invalidCallStreak = 0;
  }
  if (meta.duplicate) next.duplicateCalls += 1;
  if (meta.semanticLoop) {
    next.semanticLoops += 1;
    next.semanticLoopStreak = state.semanticLoopStreak + 1;
  } else {
    next.semanticLoopStreak = 0;
  }

  const result = obs.result ?? null;
  applyObservation(next, obs, meta, result);

  recordError(next, obs);
  if (result?.ok && !isGuardResult(result)) next.lastActionSeq = obs.seq;
  next.phase = phaseFor(next, obs);
  return next;
}

export function reduceFinalAttempt(state: StructuredTaskState, final: FinalObservation): StructuredTaskState {
  const finals = [...state.finals.filter((f) => f.seq !== final.seq), final].sort((a, b) => a.seq - b.seq);
  let phase: TaskPhase = "completing";
  if (final.accepted) {
    phase = "done";
  } else if (state.semanticLoopStreak >= 3) {
    phase = "stalled";
  }
  const next: StructuredTaskState = {
    ...state,
    finals,
    finalAttempts: state.finalAttempts + 1,
    phase,
  };
  return next;
}

/** Derives structured state from a deterministic replay of the run. */
export function stateFromRun(run: ReliabilityRun): StructuredTaskState {
  return deriveStateWithMeta(run, replayMeta(run.observations));
}

/** Derives structured state from observations plus externally supplied meta. */
export function deriveStateWithMeta(run: ReliabilityRun, meta: readonly ObservationMeta[]): StructuredTaskState {
  if (meta.length !== run.observations.length) {
    throw new Error("meta and observations must have the same length");
  }
  let state = { ...emptyTaskState(), modelCalls: run.modelCalls };
  run.observations.forEach((obs, index) => {
    state = reduceToolObservation(state, obs, meta[index]);
  });
  for (const final of run.finals) state = reduceFinalAttempt(state, final);
  return state;
}

/**
 * Code-unit ascending order — the exact order `Array.prototype.sort` applies by
 * default, stated explicitly so the canonical contract stays deterministic and
 * locale independent (paths such as `README.md` must keep sorting before
 * `docs/x`, which locale-aware collation would reverse).
 */
const compareCodeUnits = (a: string, b: string): number => {
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
};

/**
 * Canonical JSON of the decision-relevant projection.  Compaction may remove
 * intermediate read outputs, analysis and old transcript pairs — this
 * projection is defined to ignore exactly those, so two views of the same run
 * have equal contracts iff they encode the same durable state.
 */
export function stateContract(state: StructuredTaskState): string {
  const projection = {
    phase: state.phase,
    plan: { items: state.plan.items, updated: state.plan.seq !== null },
    reads: state.reads.map((r) => ({ path: r.path, digest: r.digest })),
    writes: state.writes.map((w) => ({
      path: w.path,
      marker: w.marker,
      add: w.add,
      verified: w.verified,
      verifiedBy: w.verifiedBy,
    })),
    errors: state.errors.map((e) => ({ code: e.code, tool: e.tool, digest: e.digest })),
    pendingVerification: state.pendingVerification.map((p) => p.path).sort(compareCodeUnits),
    unresolvedCommands: state.unresolvedCommands.map((c) => c.command).sort(compareCodeUnits),
    unresolvedEdits: state.unresolvedEdits.map((e) => e.path).sort(compareCodeUnits),
    lastActionHadSuccess: state.lastActionSeq > 0,
    finalAttempts: state.finalAttempts,
    finals: state.finals.map((f) => ({ content: f.content, accepted: f.accepted })),
  };
  return JSON.stringify(projection);
}

/** Deterministic multi-line summary of the structured state (model-facing). */
export function renderStateSummary(state: StructuredTaskState): string {
  const planLabel = state.plan.seq === null ? "not set" : `${state.plan.items.length} item(s)`;
  const renderWrite = (w: WriteRecord): string => {
    const verificationLabel = w.verified ? ` (verified by ${w.verifiedBy})` : " (unverified)";
    return `${w.path}${verificationLabel}`;
  };
  const writesLabel = state.writes.length === 0 ? "none" : state.writes.map(renderWrite).join("; ");
  const pendingLabel = state.pendingVerification.length === 0 ? "none" : state.pendingVerification.map((p) => p.path).join(", ");
  const failuresLabel = state.errors.length === 0 ? "none" : state.errors.map((e) => `${e.tool}:${e.code}`).join(", ");
  const loopLabel = state.semanticLoopStreak >= 3 ? `active (streak ${state.semanticLoopStreak})` : "none";
  const lines: string[] = [
    `phase: ${state.phase}`,
    `model calls: ${state.modelCalls}`,
    `tool calls: ${state.toolCalls} (invalid: ${state.invalidCalls}, duplicates: ${state.duplicateCalls})`,
    `plan: ${planLabel}`,
    `writes: ${writesLabel}`,
    `pending verification: ${pendingLabel}`,
    `unresolved: ${state.unresolvedCommands.length} command(s), ${state.unresolvedEdits.length} edit(s)`,
    `recent failures: ${failuresLabel}`,
    `loop: ${loopLabel}`,
  ];
  return lines.join("\n");
}
