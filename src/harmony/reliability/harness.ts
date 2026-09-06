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

import {
  buildCerebrasHarmonyRequest,
  type BuiltHarmonyRequest,
  type HarmonyTransport,
  normalizeHarmonyChatCompletion,
} from "../adapter.ts";
import { appendTurn, appendUser, type Conversation, createConversation } from "../conversation.ts";
import type { HarmonyReasoningEffort, NormalizedAssistantResponse, ToolCall, ToolDefinition } from "../types.ts";
import type { ToolBackends } from "../tools/backend.ts";
import { type ToolErrorCode, toolFailure, type ToolResult } from "../tools/result.ts";
import { runTool } from "../tools/router.ts";
import { toolDefinitions } from "../tools/schemas.ts";
import {
  compactTranscript,
  type ContextBudgetKind,
  estimateRequestTokens,
  renderStructuredContext,
  serializeToolResultContent,
} from "./context.ts";
import { classifyReliability, type ReliabilityClassification } from "./failure.ts";
import { invalidCallLabel, renderValidationFeedback, validateToolArgumentsDetailed } from "./feedback.ts";
import { callIdentity, LoopDetector, renderLoopFeedback } from "./loops.ts";
import {
  decideRetry,
  DEFAULT_RETRY_POLICY,
  renderRepeatedFailureFeedback,
  RetryLedger,
  type RetryPolicy,
} from "./retry.ts";
import {
  emptyTaskState,
  type FinalObservation,
  reduceFinalAttempt,
  reduceToolObservation,
  type StructuredTaskState,
  type TaskPhase,
} from "./state.ts";
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

const isTransientHttpStatus = (status: number): boolean =>
  status === 408 || status === 425 || status === 429 || status >= 500;

export type HarnessEvent =
  | Readonly<
    {
      type: "model_request";
      id: number;
      mode: "full" | "structured";
      built: BuiltHarmonyRequest;
      estimatedTokens: number;
    }
  >
  | Readonly<
    { type: "model_response"; requestId: number; normalized: NormalizedAssistantResponse; estimatedTokens: number }
  >
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

export interface HarnessOptions {
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
}

export interface HarnessOutcome {
  phase: "completed" | "failed" | "aborted";
  finalContent: string | null;
  conversation: Conversation;
  state: StructuredTaskState;
  classification: ReliabilityClassification;
  events: readonly HarnessEvent[];
  modelCalls: number;
  abortedReason: string | null;
}

const TAIL_TURNS_FOR_BUDGET: Readonly<Record<ContextBudgetKind, number>> = {
  short: 2,
  medium: 4,
  large: 8,
};

/** Deterministic model-facing policy preamble (fixed text, no secrets). */
export const renderCanonicalPolicy = (
  opts: Readonly<{ tools: readonly string[]; budget: ContextBudgetKind }>,
): string =>
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

/**
 * Runs the canonical reliability loop to completion (or a deterministic
 * failure).  Every attempt is emitted through {@link HarnessOptions.emit} and
 * persisted in {@link HarnessOutcome.events}; the authoritative conversation
 * is kept in full and only the model-facing view is compacted.
 */
export async function runReliabilityHarness(opts: HarnessOptions): Promise<HarnessOutcome> {
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
  const events: HarnessEvent[] = [];
  const emit = (event: HarnessEvent): void => {
    events.push(event);
    opts.emit?.(event);
  };
  const sleep = (ms: number): Promise<void> => {
    if (ms <= 0 || opts.signal?.aborted) return Promise.resolve();
    return new Promise((resolve) => {
      let settled = false;
      const onAbort = (): void => done();
      function done(): void {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        opts.signal?.removeEventListener("abort", onAbort);
        resolve();
      }
      const timer = setTimeout(done, ms);
      opts.signal?.addEventListener("abort", onAbort, { once: true });
      if (opts.signal?.aborted) done();
    });
  };

  let conversation = createConversation();
  if (opts.systemPrompt) conversation = appendTurn(conversation, { role: "system", content: opts.systemPrompt });
  conversation = appendUser(conversation, opts.userPrompt);

  let state: StructuredTaskState = emptyTaskState();
  let seq = 0;
  let modelCalls = 0;
  let requestCounter = 0;
  let invalidStreak = 0;
  let guardRejections = 0;
  let finalAttempts = 0;
  let emittedToolCalls = 0;
  let loopGuardEmitted = false;
  const finals: FinalObservation[] = [];
  const finalAttemptLog: FinalAttempt[] = [];

  const classify = (abortedReason: string | null): ReliabilityClassification =>
    classifyReliability({
      state,
      invalidCallStreak: invalidStreak,
      loopStreak: state.semanticLoopStreak,
      guardRejections,
      finalAccepted: finals.some((f) => f.accepted),
      abortedReason,
    });

  const abort = (reason: string): HarnessOutcome => ({
    phase: reason === "signal" ? "aborted" : "failed",
    finalContent: null,
    conversation,
    state,
    classification: classify(reason),
    events,
    modelCalls,
    abortedReason: reason,
  });

  const requestConversationFor = (): Conversation => {
    if (mode === "full") return compactTranscript(conversation, { budget }).conversation;
    const text = renderStructuredContext(state, conversation, {
      maxTailTurns: TAIL_TURNS_FOR_BUDGET[budget],
    });
    const head = conversation.turns.filter((turn) => turn.role === "system" || turn.role === "developer");
    return createConversation([...head, { role: "user", content: `${opts.userPrompt}\n\n${text}` }]);
  };

  const appendToolPair = (call: ToolCall, result: ToolResult, analysis: readonly string[]): void => {
    conversation = appendTurn(conversation, assistantTurn(call, analysis));
    conversation = appendTurn(conversation, {
      role: "tool",
      toolCallId: call.id,
      name: call.name,
      content: serializeToolResultContent(result),
    });
  };

  for (let turn = 0; turn < maxTurns; turn++) {
    if (opts.signal?.aborted) return abort("signal");
    if (emittedToolCalls >= maxToolCalls) return abort("tool_call_limit");

    let built: BuiltHarmonyRequest;
    try {
      built = buildCerebrasHarmonyRequest({
        style: "generic",
        turns: requestConversationFor().turns,
        tools,
        reasoningEffort: opts.reasoningEffort ?? "low",
        maxCompletionTokens,
      });
    } catch {
      return abort("invalid_config");
    }

    // Transport with deterministic retry (transient only).
    let normalized: NormalizedAssistantResponse | null = null;
    for (let attempt = 0; attempt <= retryPolicy.maxRetriesPerCall; attempt++) {
      if (opts.signal?.aborted) return abort("signal");
      requestCounter += 1;
      const requestId = requestCounter;
      emit({ type: "model_request", id: requestId, mode, built, estimatedTokens: estimateRequestTokens(built.body) });
      let response: Response;
      try {
        response = await opts.transport(built.body, { signal: opts.signal });
      } catch {
        if (opts.signal?.aborted) return abort("signal");
        await sleep(retryPolicy.backoffMs);
        continue;
      }
      if (opts.signal?.aborted) return abort("signal");
      if (!response.ok) {
        await response.body?.cancel().catch(() => undefined);
        if (opts.signal?.aborted) return abort("signal");
        if (!isTransientHttpStatus(response.status)) break;
        await sleep(retryPolicy.backoffMs);
        continue;
      }
      const body = await response.json().catch(() => null);
      if (opts.signal?.aborted) return abort("signal");
      if (body === null) {
        await sleep(retryPolicy.backoffMs);
        continue;
      }
      const candidate = normalizeHarmonyChatCompletion(body);
      if ("error" in candidate) {
        await sleep(retryPolicy.backoffMs);
        continue;
      }
      normalized = candidate;
      emit({
        type: "model_response",
        requestId,
        normalized,
        estimatedTokens: Math.max(
          1,
          Math.ceil(
            ((normalized.content ?? "").length +
              normalized.toolCalls.reduce((n, call) => n + call.arguments.length, 0)) / 4,
          ),
        ),
      });
      break;
    }
    if (normalized === null) return abort("transport_failed");
    modelCalls += 1;
    state = { ...state, modelCalls };

    // --- Final answer attempt.
    if (normalized.toolCalls.length === 0) {
      const content = normalized.content ?? "";
      if (content.trim() === "") return abort("no_model_output");
      finalAttempts += 1;
      const decision = guardFinal({
        finalContent: content,
        lastActionSeq: state.lastActionSeq,
        previousFinals: finalAttemptLog,
        semanticLoopStreak: state.semanticLoopStreak,
        planUpdated: state.plan.seq !== null,
        writes: state.writes.length,
        tracker,
        policy: verificationPolicy,
      });
      finalAttemptLog.push(decision.attempt);
      finals.push({ content, accepted: decision.allowed, seq: seq + 1 });
      conversation = appendTurn(conversation, {
        role: "assistant",
        content,
        analysis: normalized.analysis,
        toolCalls: [],
        finishReason: normalized.finishReason,
      });
      state = reduceFinalAttempt(state, { content, accepted: decision.allowed, seq: seq + 1 });
      if (decision.allowed) {
        emit({ type: "final", content, accepted: true, attempt: finalAttempts });
        return {
          phase: "completed",
          finalContent: content,
          conversation,
          state,
          classification: classify(null),
          events,
          modelCalls,
          abortedReason: null,
        };
      }
      emit({ type: "final", content, accepted: false, attempt: finalAttempts });
      guardRejections += 1;
      const first = decision.requirements[0];
      emit({
        type: "guard",
        kind: decision.falseCompletion ? "false_completion" : first?.kind ?? "unverified_write",
        message: renderGuardRequirements(decision.requirements),
        attempt: finalAttempts,
        phase: state.phase,
      });
      conversation = appendUser(conversation, `${GUARD_PREFIX}: ${renderGuardRequirements(decision.requirements)}`);
      if (decision.falseCompletion && decision.attempt.repetitions >= verificationPolicy.maxRepeatedFinals) {
        return abort("false_completion");
      }
      if (finalAttempts >= verificationPolicy.maxFinalAttempts) return abort("guard_exhausted");
      if (guardRejections >= maxGuardRejections) return abort("guard_exhausted");
      continue;
    }

    // --- Tool calls (parallel calls are processed sequentially in order).
    for (const call of normalized.toolCalls) {
      if (emittedToolCalls >= maxToolCalls) return abort("tool_call_limit");
      emittedToolCalls += 1;
      seq += 1;
      const id = `t${seq}`;
      const validation = validateToolArgumentsDetailed(call.name, parseArguments(call.arguments));

      if (!validation.valid) {
        // Deterministic feedback; the call is never executed.
        const result = toolFailure("invalid_args", renderValidationFeedback(call.name, validation));
        const flags = detector.observe(call.name, validation.arguments, result);
        emit({
          type: "tool_call",
          id,
          tool: call.name,
          arguments: validation.arguments,
          valid: false,
          invalidReason: invalidCallLabel(validation),
        });
        emit({ type: "tool_result", id, result });
        appendToolPair(call, result, normalized.analysis);
        invalidStreak += 1;
        if (invalidStreak >= invalidCallStreakLimit) return abort("invalid_argument_loop");
        state = reduceToolObservation(state, {
          seq,
          tool: call.name,
          args: validation.arguments,
          valid: false,
          result,
        }, { duplicate: null, semanticLoop: flags.semanticLoop, verification: null });
        continue;
      }

      const identity = callIdentity(call.name, validation.arguments);
      const priorAttempts = retryLedger.priorAttempts(identity);
      const previousCode = retryLedger.entry(identity)?.lastCode ?? null;
      const duplicate = detector.checkDuplicate(call.name, validation.arguments);
      const retryDecision = duplicate !== null && priorAttempts > 0
        ? decideRetry(retryPolicy, previousCode, priorAttempts)
        : null;

      let result: ToolResult;
      if (duplicate !== null && (retryDecision === null || !retryDecision.retry)) {
        // Deterministic guard: never re-execute an identical call.
        const blockedCode = duplicate === "repeat_after_success" ? "duplicate_call" : "repeated_failure";
        const message = blockedCode === "duplicate_call"
          ? `duplicate of the previous call ${call.name}(${JSON.stringify(validation.arguments)}); ` +
            "do not repeat it — read the existing result or take a different action"
          : renderRepeatedFailureFeedback(previousCode, identity);
        result = { ok: false, error: message, error_code: blockedCode as ToolErrorCode };
        emit({
          type: "tool_call",
          id,
          tool: call.name,
          arguments: validation.arguments,
          valid: true,
          repeated: duplicate,
        });
        emit({ type: "tool_result", id, result });
        appendToolPair(call, result, normalized.analysis);
      } else {
        if (retryDecision?.retry === true && retryDecision.delayMs > 0) await sleep(retryDecision.delayMs);
        emit({
          type: "tool_call",
          id,
          tool: call.name,
          arguments: validation.arguments,
          valid: true,
          repeated: duplicate ?? null,
        });
        const started = Date.now();
        try {
          result = await runTool(opts.backends, call.name, validation.arguments, { signal: opts.signal });
        } catch (err) {
          result = toolFailure("internal", err instanceof Error ? err.message : String(err));
        }
        emit({ type: "tool_result", id, result, durationMs: Date.now() - started });
        appendToolPair(call, result, normalized.analysis);
      }

      retryLedger.observe(identity, result, priorAttempts);
      const verification = tracker.observe(call.name, validation.arguments, result);
      const flags = detector.observe(call.name, validation.arguments, result);
      state = reduceToolObservation(state, {
        seq,
        tool: call.name,
        args: validation.arguments,
        valid: true,
        result,
      }, { duplicate: flags.duplicate ?? duplicate ?? null, semanticLoop: flags.semanticLoop, verification });
      invalidStreak = 0;
      if (flags.semanticLoop && flags.streak >= loopThreshold && !loopGuardEmitted) {
        loopGuardEmitted = true;
        emit({ type: "guard", kind: "loop", message: renderLoopFeedback(flags), attempt: 0, phase: state.phase });
      }
      if (!flags.semanticLoop) loopGuardEmitted = false;
    }
  }
  return abort("turn_limit");
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
