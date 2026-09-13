/**
 * Canonical reliability harness (plan m05).
 *
 * {@link runReliabilityHarness} is the deterministic agent loop behind
 * benchmark config C.  It composes the m01 adapter primitives (request
 * building, transport, normalization), the m04 canonical tool router and the
 * m05 reliability layers:
 *
 * - detailed argument validation with complete corrective feedback
 *   (`feedback.ts`) — invalid calls are never executed;
 * - duplicate / semantic-loop detection (`loops.ts`) with deterministic
 *   guard envelopes (`duplicate_call`, `repeated_failure`) that are recorded
 *   in the transcript but never counted as tool errors;
 * - retry policy (`retry.ts`) — identical calls after *transient* failures
 *   may be retried with backoff; identical calls after deterministic
 *   failures or successes are blocked, never re-executed;
 * - verification requirements (`verify.ts`) — unverified writes, unresolved
 *   command/edit failures and active loops block the final answer
 *   (false-completion prevention); the model receives one deterministic
 *   `[guard]` message per rejection;
 * - structured state (`state.ts`) and transcript compaction / structured
 *   context (`context.ts`) for short/medium/large budgets.
 *
 * The transport and tool backends are injected: no environment variable, no
 * CLI flag, no secret, no external call happens here.  Fake transports drive
 * the same loop in focused tests and in the C-fake benchmark matrix.
 */

import { buildCerebrasHarmonyRequest, type BuiltHarmonyRequest, type HarmonyTransport, normalizeHarmonyChatCompletion } from "../adapter.ts";
import { appendTurn, appendUser, type Conversation, createConversation } from "../conversation.ts";
import type { HarmonyReasoningEffort, NormalizedAssistantResponse, ToolCall, ToolDefinition } from "../types.ts";
import type { ToolBackends } from "../tools/backend.ts";
import { type ToolErrorCode, toolFailure, type ToolResult } from "../tools/result.ts";
import { runTool } from "../tools/router.ts";
import { toolDefinitions } from "../tools/schemas.ts";
import { compactTranscript, type ContextBudgetKind, estimateRequestTokens, renderStructuredContext, serializeToolResultContent } from "./context.ts";
import { classifyReliability, type ReliabilityClassification } from "./failure.ts";
import { invalidCallLabel, renderValidationFeedback, validateToolArgumentsDetailed, type DetailedValidationResult } from "./feedback.ts";
import { callIdentity, type DuplicateFlag, LoopDetector, renderLoopFeedback } from "./loops.ts";
import { decideRetry, DEFAULT_RETRY_POLICY, renderRepeatedFailureFeedback, RetryLedger, type RetryDecision, type RetryPolicy } from "./retry.ts";
import { emptyTaskState, type FinalObservation, reduceFinalAttempt, reduceToolObservation, type StructuredTaskState, type TaskPhase } from "./state.ts";
import {
  DEFAULT_VERIFICATION_POLICY,
  type FinalAttempt,
  type FinalRequirementKind,
  GUARD_PREFIX,
  guardFinal,
  renderGuardRequirements,
  type VerificationPolicy,
  VerificationTracker,
} from "./verify.ts";

const isTransientHttpStatus = (status: number): boolean => status === 408 || status === 425 || status === 429 || status >= 500;

export type HarnessEvent =
  | Readonly<{
      type: "model_request";
      id: number;
      mode: "full" | "structured";
      built: BuiltHarmonyRequest;
      estimatedTokens: number;
    }>
  | Readonly<{ type: "model_response"; requestId: number; normalized: NormalizedAssistantResponse; estimatedTokens: number }>
  | Readonly<{
      type: "tool_call";
      id: string;
      tool: string;
      arguments: Record<string, unknown>;
      valid: boolean;
      invalidReason?: string;
      repeated?: string | null;
    }>
  | Readonly<{ type: "tool_result"; id: string; result: ToolResult; durationMs?: number }>
  | Readonly<{ type: "guard"; kind: FinalRequirementKind | "loop"; message: string; attempt: number; phase: TaskPhase }>
  | Readonly<{ type: "final"; content: string; accepted: boolean; attempt: number }>;

export type HarnessOptions = {
  systemPrompt: string;
  userPrompt: string;
  transport: HarmonyTransport;
  backends: ToolBackends;
  /** Model-facing tool surface; defaults to the canonical compact surface. */
  tools?: readonly ToolDefinition[];
  reasoningEffort?: HarmonyReasoningEffort;
  maxCompletionTokens?: number;
  maxTurns?: number;
  maxToolCalls?: number;
  invalidCallStreakLimit?: number;
  loopThreshold?: number;
  maxGuardRejections?: number;
  transcriptBudget?: ContextBudgetKind;
  contextMode?: "full" | "structured";
  retryPolicy?: RetryPolicy;
  verificationPolicy?: VerificationPolicy;
  /** Task-declared verification command (exact-match exec satisfies writes). */
  verificationCommand?: string | null;
  emit?: (event: HarnessEvent) => void;
  signal?: AbortSignal;
};

export type HarnessOutcome = {
  phase: "completed" | "failed" | "aborted";
  finalContent: string | null;
  conversation: Conversation;
  state: StructuredTaskState;
  classification: ReliabilityClassification;
  events: readonly HarnessEvent[];
  modelCalls: number;
  abortedReason: string | null;
};

const TAIL_TURNS_FOR_BUDGET: Readonly<Record<ContextBudgetKind, number>> = {
  short: 2,
  medium: 4,
  large: 8,
};

/** Deterministic model-facing policy preamble (fixed text, no secrets). */
export const renderCanonicalPolicy = (opts: Readonly<{ tools: readonly string[]; budget: ContextBudgetKind }>): string =>
  [
    "You are a deterministic agent running inside the canonical reliability harness.",
    "Rules:",
    "- Validate the exact tool arguments before calling a tool. Invalid calls are not executed and you must correct them.",
    "- Never repeat a tool call with identical arguments: repeats are detected and blocked. After a failure, change the arguments or the approach.",
    "- After every edit, verify it (read the file back or run a check command) before answering.",
    "- Never claim completion while a verification is pending or a command/editing failure is unresolved.",
    `- Available tools: ${opts.tools.join(", ")}.`,
    `- Context budget tier: ${opts.budget}.`,
    "- Answer only when everything is verified.",
  ].join("\n");

/** Mutable state of one harness run: resolved options, counters, transcript. */
type HarnessRun = {
  readonly tools: readonly ToolDefinition[];
  readonly retryPolicy: RetryPolicy;
  readonly verificationPolicy: VerificationPolicy;
  readonly budget: ContextBudgetKind;
  readonly mode: "full" | "structured";
  readonly maxTurns: number;
  readonly maxToolCalls: number;
  readonly invalidCallStreakLimit: number;
  readonly loopThreshold: number;
  readonly maxGuardRejections: number;
  readonly maxCompletionTokens: number;
  readonly detector: LoopDetector;
  readonly tracker: VerificationTracker;
  readonly retryLedger: RetryLedger;
  readonly events: HarnessEvent[];
  readonly finals: FinalObservation[];
  readonly finalAttemptLog: FinalAttempt[];
  conversation: Conversation;
  state: StructuredTaskState;
  seq: number;
  modelCalls: number;
  requestCounter: number;
  invalidStreak: number;
  guardRejections: number;
  finalAttempts: number;
  emittedToolCalls: number;
  loopGuardEmitted: boolean;
};

/** Every deterministic abort reason the loop can report. */
type HarnessAbortReason =
  | "signal"
  | "tool_call_limit"
  | "invalid_config"
  | "transport_failed"
  | "no_model_output"
  | "invalid_argument_loop"
  | "false_completion"
  | "guard_exhausted"
  | "turn_limit";

/** One transport attempt outcome inside the bounded retry loop. */
type ModelTransportAttempt =
  | Readonly<{ kind: "aborted" }>
  | Readonly<{ kind: "retry" }>
  | Readonly<{ kind: "stop" }>
  | Readonly<{ kind: "response"; normalized: NormalizedAssistantResponse }>;

/** Resolves every option default and seeds the transcript and counters. */
const createHarnessRun = (opts: HarnessOptions): HarnessRun => {
  const tools = opts.tools ?? toolDefinitions();
  const retryPolicy = opts.retryPolicy ?? DEFAULT_RETRY_POLICY;
  const verificationPolicy: VerificationPolicy = {
    ...DEFAULT_VERIFICATION_POLICY,
    ...opts.verificationPolicy,
    verificationCommand: opts.verificationPolicy?.verificationCommand ?? opts.verificationCommand ?? null,
  };
  const budget = opts.transcriptBudget ?? "medium";
  const mode = opts.contextMode ?? "structured";
  const maxTurns = opts.maxTurns ?? 40;
  const maxToolCalls = opts.maxToolCalls ?? 60;
  const invalidCallStreakLimit = opts.invalidCallStreakLimit ?? 4;
  const loopThreshold = opts.loopThreshold ?? 3;
  const maxGuardRejections = opts.maxGuardRejections ?? 6;
  const maxCompletionTokens = opts.maxCompletionTokens ?? 512;

  const detector = new LoopDetector();
  const tracker = new VerificationTracker(verificationPolicy);
  const retryLedger = new RetryLedger(retryPolicy);

  let conversation = createConversation();
  if (opts.systemPrompt) conversation = appendTurn(conversation, { role: "system", content: opts.systemPrompt });
  conversation = appendUser(conversation, opts.userPrompt);

  return {
    tools,
    retryPolicy,
    verificationPolicy,
    budget,
    mode,
    maxTurns,
    maxToolCalls,
    invalidCallStreakLimit,
    loopThreshold,
    maxGuardRejections,
    maxCompletionTokens,
    detector,
    tracker,
    retryLedger,
    events: [],
    finals: [],
    finalAttemptLog: [],
    conversation,
    state: emptyTaskState(),
    seq: 0,
    modelCalls: 0,
    requestCounter: 0,
    invalidStreak: 0,
    guardRejections: 0,
    finalAttempts: 0,
    emittedToolCalls: 0,
    loopGuardEmitted: false,
  };
};

/** Records one event in the run and forwards it to the caller's sink. */
const emitHarnessEvent = (run: HarnessRun, opts: HarnessOptions, event: HarnessEvent): void => {
  run.events.push(event);
  opts.emit?.(event);
};

/** Deterministic backoff sleep (a zero delay resolves without a timer). */
const sleepMs = (ms: number): Promise<void> => (ms > 0 ? new Promise((resolve) => setTimeout(resolve, ms)) : Promise.resolve());

/** Advisory reliability classification of the state reached so far. */
const classifyRun = (run: HarnessRun, abortedReason: string | null): ReliabilityClassification =>
  classifyReliability({
    state: run.state,
    invalidCallStreak: run.invalidStreak,
    loopStreak: run.state.semanticLoopStreak,
    guardRejections: run.guardRejections,
    finalAccepted: run.finals.some((f) => f.accepted),
    abortedReason,
  });

/** Terminates the run with a deterministic failure/abort outcome. */
const abortRun = (run: HarnessRun, reason: HarnessAbortReason): HarnessOutcome => ({
  phase: reason === "signal" ? "aborted" : "failed",
  finalContent: null,
  conversation: run.conversation,
  state: run.state,
  classification: classifyRun(run, reason),
  events: run.events,
  modelCalls: run.modelCalls,
  abortedReason: reason,
});

/** Terminates the run with an accepted final answer. */
const completeRun = (run: HarnessRun, content: string): HarnessOutcome => ({
  phase: "completed",
  finalContent: content,
  conversation: run.conversation,
  state: run.state,
  classification: classifyRun(run, null),
  events: run.events,
  modelCalls: run.modelCalls,
  abortedReason: null,
});

/** The model-facing conversation of the next request (full or structured). */
const requestConversationFor = (run: HarnessRun, opts: HarnessOptions): Conversation => {
  if (run.mode === "full") return compactTranscript(run.conversation, { budget: run.budget }).conversation;
  const text = renderStructuredContext(run.state, run.conversation, {
    maxTailTurns: TAIL_TURNS_FOR_BUDGET[run.budget],
  });
  const head = run.conversation.turns.filter((turn) => turn.role === "system" || turn.role === "developer");
  return createConversation([...head, { role: "user", content: `${opts.userPrompt}\n\n${text}` }]);
};

/** Appends the assistant tool-call turn and its recorded tool result. */
const appendToolPair = (run: HarnessRun, call: ToolCall, result: ToolResult, analysis: readonly string[]): void => {
  run.conversation = appendTurn(run.conversation, assistantTurn(call, analysis));
  run.conversation = appendTurn(run.conversation, {
    role: "tool",
    toolCallId: call.id,
    name: call.name,
    content: serializeToolResultContent(result),
  });
};

/** Builds one Harmony request, or null when the configuration is unusable. */
const buildHarnessRequest = (run: HarnessRun, opts: HarnessOptions): BuiltHarmonyRequest | null => {
  try {
    return buildCerebrasHarmonyRequest({
      style: "generic",
      turns: requestConversationFor(run, opts).turns,
      tools: run.tools,
      reasoningEffort: opts.reasoningEffort ?? "low",
      maxCompletionTokens: run.maxCompletionTokens,
    });
  } catch {
    return null;
  }
};

/** One transport attempt, classified for the bounded retry loop. */
const attemptModelTransport = async (run: HarnessRun, opts: HarnessOptions, built: BuiltHarmonyRequest): Promise<ModelTransportAttempt> => {
  run.requestCounter += 1;
  const requestId = run.requestCounter;
  emitHarnessEvent(run, opts, { type: "model_request", id: requestId, mode: run.mode, built, estimatedTokens: estimateRequestTokens(built.body) });
  let response: Response;
  try {
    response = await opts.transport(built.body, { signal: opts.signal });
  } catch {
    if (opts.signal?.aborted) return { kind: "aborted" };
    await sleepMs(run.retryPolicy.backoffMs);
    return { kind: "retry" };
  }
  if (!response.ok) {
    await response.body?.cancel().catch(() => undefined);
    if (!isTransientHttpStatus(response.status)) return { kind: "stop" };
    await sleepMs(run.retryPolicy.backoffMs);
    return { kind: "retry" };
  }
  const body = await response.json().catch(() => null);
  if (body === null) {
    await sleepMs(run.retryPolicy.backoffMs);
    return { kind: "retry" };
  }
  const normalized = normalizeHarmonyChatCompletion(body);
  if ("error" in normalized) {
    await sleepMs(run.retryPolicy.backoffMs);
    return { kind: "retry" };
  }
  emitHarnessEvent(run, opts, {
    type: "model_response",
    requestId,
    normalized,
    estimatedTokens: Math.max(1, Math.ceil(((normalized.content ?? "").length + normalized.toolCalls.reduce((n, call) => n + call.arguments.length, 0)) / 4)),
  });
  return { kind: "response", normalized };
};

/** Transport with deterministic retry (transient failures only). */
const requestModelResponse = async (run: HarnessRun, opts: HarnessOptions, built: BuiltHarmonyRequest): Promise<NormalizedAssistantResponse | null> => {
  for (let attempt = 0; attempt <= run.retryPolicy.maxRetriesPerCall; attempt++) {
    if (opts.signal?.aborted) return null;
    const outcome = await attemptModelTransport(run, opts, built);
    if (outcome.kind === "aborted") return null;
    if (outcome.kind === "response") return outcome.normalized;
    if (outcome.kind === "stop") break;
  }
  return null;
};

/** Deterministic guard envelope for an identical repeat that must not run. */
const blockedDuplicateResult = (
  call: ToolCall,
  args: Record<string, unknown>,
  duplicate: DuplicateFlag,
  previousCode: string | null,
  identity: string
): ToolResult => {
  const blockedCode = duplicate === "repeat_after_success" ? "duplicate_call" : "repeated_failure";
  const message =
    blockedCode === "duplicate_call"
      ? `duplicate of the previous call ${call.name}(${JSON.stringify(args)}); ` + "do not repeat it — read the existing result or take a different action"
      : renderRepeatedFailureFeedback(previousCode, identity);
  return { ok: false, error: message, error_code: blockedCode as ToolErrorCode };
};

/** Records a guarded duplicate call without executing it. */
const recordBlockedDuplicate = (
  run: HarnessRun,
  opts: HarnessOptions,
  id: string,
  call: ToolCall,
  args: Record<string, unknown>,
  analysis: readonly string[],
  duplicate: DuplicateFlag,
  previousCode: string | null,
  identity: string
): ToolResult => {
  // Deterministic guard: never re-execute an identical call.
  const result = blockedDuplicateResult(call, args, duplicate, previousCode, identity);
  emitHarnessEvent(run, opts, { type: "tool_call", id, tool: call.name, arguments: args, valid: true, repeated: duplicate });
  emitHarnessEvent(run, opts, { type: "tool_result", id, result });
  appendToolPair(run, call, result, analysis);
  return result;
};

/** Executes one validated call through the canonical tool router. */
const executeToolCall = async (
  run: HarnessRun,
  opts: HarnessOptions,
  id: string,
  call: ToolCall,
  args: Record<string, unknown>,
  analysis: readonly string[],
  duplicate: DuplicateFlag | null,
  retryDecision: RetryDecision | null
): Promise<ToolResult> => {
  if (retryDecision?.retry === true && retryDecision.delayMs > 0) await sleepMs(retryDecision.delayMs);
  emitHarnessEvent(run, opts, { type: "tool_call", id, tool: call.name, arguments: args, valid: true, repeated: duplicate ?? null });
  const started = Date.now();
  let result: ToolResult;
  try {
    result = await runTool(opts.backends, call.name, args, { signal: opts.signal });
  } catch (err) {
    result = toolFailure("internal", err instanceof Error ? err.message : String(err));
  }
  emitHarnessEvent(run, opts, { type: "tool_result", id, result, durationMs: Date.now() - started });
  appendToolPair(run, call, result, analysis);
  return result;
};

/** Records one invalid call: deterministic feedback, never executed. */
const recordInvalidToolCall = (
  run: HarnessRun,
  opts: HarnessOptions,
  id: string,
  call: ToolCall,
  validation: DetailedValidationResult,
  analysis: readonly string[]
): HarnessOutcome | null => {
  // Deterministic feedback; the call is never executed.
  const result = toolFailure("invalid_args", renderValidationFeedback(call.name, validation));
  const flags = run.detector.observe(call.name, validation.arguments, result);
  emitHarnessEvent(run, opts, {
    type: "tool_call",
    id,
    tool: call.name,
    arguments: validation.arguments,
    valid: false,
    invalidReason: invalidCallLabel(validation),
  });
  emitHarnessEvent(run, opts, { type: "tool_result", id, result });
  appendToolPair(run, call, result, analysis);
  run.invalidStreak += 1;
  if (run.invalidStreak >= run.invalidCallStreakLimit) return abortRun(run, "invalid_argument_loop");
  run.state = reduceToolObservation(
    run.state,
    {
      seq: run.seq,
      tool: call.name,
      args: validation.arguments,
      valid: false,
      result,
    },
    { duplicate: null, semanticLoop: flags.semanticLoop, verification: null }
  );
  return null;
};

/** Processes one model tool call; a non-null result terminates the run. */
const processToolCall = async (run: HarnessRun, opts: HarnessOptions, call: ToolCall, analysis: readonly string[]): Promise<HarnessOutcome | null> => {
  const id = `t${run.seq}`;
  const validation = validateToolArgumentsDetailed(call.name, parseArguments(call.arguments));

  if (!validation.valid) return recordInvalidToolCall(run, opts, id, call, validation, analysis);

  const identity = callIdentity(call.name, validation.arguments);
  const priorAttempts = run.retryLedger.priorAttempts(identity);
  const previousCode = run.retryLedger.entry(identity)?.lastCode ?? null;
  const duplicate = run.detector.checkDuplicate(call.name, validation.arguments);
  const retryDecision = duplicate !== null && priorAttempts > 0 ? decideRetry(run.retryPolicy, previousCode, priorAttempts) : null;
  const result =
    duplicate !== null && !retryDecision?.retry
      ? recordBlockedDuplicate(run, opts, id, call, validation.arguments, analysis, duplicate, previousCode, identity)
      : await executeToolCall(run, opts, id, call, validation.arguments, analysis, duplicate, retryDecision);

  run.retryLedger.observe(identity, result, priorAttempts);
  const verification = run.tracker.observe(call.name, validation.arguments, result);
  const flags = run.detector.observe(call.name, validation.arguments, result);
  run.state = reduceToolObservation(
    run.state,
    {
      seq: run.seq,
      tool: call.name,
      args: validation.arguments,
      valid: true,
      result,
    },
    { duplicate: flags.duplicate ?? duplicate ?? null, semanticLoop: flags.semanticLoop, verification }
  );
  run.invalidStreak = 0;
  if (flags.semanticLoop && flags.streak >= run.loopThreshold && !run.loopGuardEmitted) {
    run.loopGuardEmitted = true;
    emitHarnessEvent(run, opts, { type: "guard", kind: "loop", message: renderLoopFeedback(flags), attempt: 0, phase: run.state.phase });
  }
  if (!flags.semanticLoop) run.loopGuardEmitted = false;
  return null;
};

/** Runs every tool call of one model response (sequentially, in order). */
const runToolCalls = async (run: HarnessRun, opts: HarnessOptions, normalized: NormalizedAssistantResponse): Promise<HarnessOutcome | null> => {
  // Tool calls (parallel calls are processed sequentially in order).
  for (const call of normalized.toolCalls) {
    if (run.emittedToolCalls >= run.maxToolCalls) return abortRun(run, "tool_call_limit");
    run.emittedToolCalls += 1;
    run.seq += 1;
    const outcome = await processToolCall(run, opts, call, normalized.analysis);
    if (outcome !== null) return outcome;
  }
  return null;
};

/** Handles a final answer attempt: guard decision, transcript and outcome. */
const runFinalAttempt = (run: HarnessRun, opts: HarnessOptions, normalized: NormalizedAssistantResponse): HarnessOutcome | null => {
  // --- Final answer attempt.
  const content = normalized.content ?? "";
  if (content.trim() === "") return abortRun(run, "no_model_output");
  run.finalAttempts += 1;
  const decision = guardFinal({
    finalContent: content,
    lastActionSeq: run.state.lastActionSeq,
    previousFinals: run.finalAttemptLog,
    semanticLoopStreak: run.state.semanticLoopStreak,
    planUpdated: run.state.plan.seq !== null,
    writes: run.state.writes.length,
    tracker: run.tracker,
    policy: run.verificationPolicy,
  });
  run.finalAttemptLog.push(decision.attempt);
  run.finals.push({ content, accepted: decision.allowed, seq: run.seq + 1 });
  run.conversation = appendTurn(run.conversation, {
    role: "assistant",
    content,
    analysis: normalized.analysis,
    toolCalls: [],
    finishReason: normalized.finishReason,
  });
  run.state = reduceFinalAttempt(run.state, { content, accepted: decision.allowed, seq: run.seq + 1 });
  if (decision.allowed) {
    emitHarnessEvent(run, opts, { type: "final", content, accepted: true, attempt: run.finalAttempts });
    return completeRun(run, content);
  }
  emitHarnessEvent(run, opts, { type: "final", content, accepted: false, attempt: run.finalAttempts });
  run.guardRejections += 1;
  // `guardFinal` reports `allowed: requirements.length === 0`, so a rejected
  // final always carries at least one blocking requirement here.
  const first = decision.requirements[0];
  emitHarnessEvent(run, opts, {
    type: "guard",
    kind: decision.falseCompletion ? "false_completion" : first.kind,
    message: renderGuardRequirements(decision.requirements),
    attempt: run.finalAttempts,
    phase: run.state.phase,
  });
  run.conversation = appendUser(run.conversation, `${GUARD_PREFIX}: ${renderGuardRequirements(decision.requirements)}`);
  if (decision.falseCompletion && decision.attempt.repetitions >= run.verificationPolicy.maxRepeatedFinals) {
    return abortRun(run, "false_completion");
  }
  if (run.finalAttempts >= run.verificationPolicy.maxFinalAttempts) return abortRun(run, "guard_exhausted");
  if (run.guardRejections >= run.maxGuardRejections) return abortRun(run, "guard_exhausted");
  return null;
};

/** Pre-turn guards: the abort signal, then the hard tool-call budget. */
const turnAbortReason = (run: HarnessRun, opts: HarnessOptions): HarnessAbortReason | null => {
  if (opts.signal?.aborted) return "signal";
  if (run.emittedToolCalls >= run.maxToolCalls) return "tool_call_limit";
  return null;
};

/** One full model turn: request, transport, then final answer or tool calls. */
const runHarnessTurn = async (run: HarnessRun, opts: HarnessOptions): Promise<HarnessOutcome | null> => {
  const abortReason = turnAbortReason(run, opts);
  if (abortReason !== null) return abortRun(run, abortReason);

  const built = buildHarnessRequest(run, opts);
  if (built === null) return abortRun(run, "invalid_config");
  const normalized = await requestModelResponse(run, opts, built);
  if (normalized === null) return abortRun(run, opts.signal?.aborted ? "signal" : "transport_failed");
  run.modelCalls += 1;
  run.state = { ...run.state, modelCalls: run.modelCalls };

  if (normalized.toolCalls.length === 0) return runFinalAttempt(run, opts, normalized);
  return await runToolCalls(run, opts, normalized);
};

/**
 * Runs the canonical reliability loop to completion (or a deterministic
 * failure).  Every attempt is emitted through {@link HarnessOptions.emit} and
 * persisted in {@link HarnessOutcome.events}; the authoritative conversation
 * is kept in full and only the model-facing view is compacted.
 */
export async function runReliabilityHarness(opts: HarnessOptions): Promise<HarnessOutcome> {
  const run = createHarnessRun(opts);
  for (let turn = 0; turn < run.maxTurns; turn++) {
    const outcome = await runHarnessTurn(run, opts);
    if (outcome !== null) return outcome;
  }
  return abortRun(run, "turn_limit");
}

function parseArguments(argumentsText: string): Record<string, unknown> | null {
  try {
    const value = JSON.parse(argumentsText);
    if (typeof value === "object" && value !== null && !Array.isArray(value)) return value as Record<string, unknown>;
  } catch {
    // fall through to the invalid-arguments path
  }
  return null;
}

function assistantTurn(call: ToolCall, analysis: readonly string[]) {
  return {
    role: "assistant" as const,
    content: null,
    analysis,
    toolCalls: [call as ToolCall],
    finishReason: "tool_calls",
  };
}
