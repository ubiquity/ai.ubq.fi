/**
 * Transcript compaction and structured context (plan m05).
 *
 * The canonical harness keeps the authoritative conversation in full and
 * compacts the MODEL-FACING view per context budget tier before every
 * request.  Three deterministic policies are defined for short / medium /
 * large contexts (larger budgets → less aggressive compaction):
 *
 * - **never dropped** (contract-critical): failed calls, `editor.apply_patch`
 *   pairs, `task.update_plan` pairs, `shell.exec` pairs (command evidence and
 *   verification), guard feedback, final answers, user/system turns;
 * - **droppable** when old or stale: successful `filesystem.read` pairs
 *   beyond `readsPerPath` per path, successful find/search/browser pairs
 *   beyond the recent window, and private analysis outside the window.
 *
 * Dropping is always WHOLE pairs (assistant call + tool result together) and
 * never changes content, which keeps the structured-state contract invariant:
 * the last read per path and every patch/plan/command pair survive, so the
 * verification replay produces identical state from the compacted view.
 *
 * Tool results are stored in the conversation as one deterministic JSON line
 * ({@link serializeToolResultContent}) so the compaction layer can classify
 * pairs without side tables; the same parse is used by the replay derivation.
 *
 * Token accounting is a deterministic surrogate (`estimateTokens`), never a
 * live usage report: it exists so fake-transport runs and the comparison
 * evidence are reproducible offline.
 */

import { type Conversation, wireMessagesFromConversation } from "../conversation.ts";
import type { ConversationTurn } from "../types.ts";
import { renderStateSummary, type StructuredTaskState } from "./state.ts";

// ---------------------------------------------------------------------------
// Deterministic token estimation (surrogate; not a live usage report)
// ---------------------------------------------------------------------------

export const estimateTokens = (text: string): number => Math.max(1, Math.ceil(text.length / 4));

export const estimateJsonTokens = (value: unknown): number => estimateTokens(JSON.stringify(value));

/** Deterministic model-facing message payload of a conversation. */
export const wireMessages = (conversation: Conversation): readonly Record<string, unknown>[] =>
  wireMessagesFromConversation(conversation);

/** Estimated tokens of a full wire transcript (messages only). */
export const estimateConversationTokens = (conversation: Conversation): number =>
  estimateJsonTokens(wireMessages(conversation));

/** Estimated tokens of one request body (messages + tools + overhead). */
export const estimateRequestTokens = (body: Readonly<Record<string, unknown>>): number => estimateJsonTokens(body) + 4;

// ---------------------------------------------------------------------------
// Machine-readable tool-result content (conversation storage format)
// ---------------------------------------------------------------------------

export interface ParsedToolResult {
  ok: boolean;
  output?: string | null;
  error?: string | null;
  error_code?: string | null;
}

/** Serializes one result envelope as the deterministic conversation line. */
export const serializeToolResultContent = (result: ParsedToolResult): string =>
  JSON.stringify({
    ok: result.ok,
    output: result.output ?? null,
    error: result.error ?? null,
    error_code: result.error_code ?? null,
  });

/** Parses a stored conversation tool-result line, or null when not canonical. */
export const parseToolResultContent = (content: string): ParsedToolResult | null => {
  try {
    const value = JSON.parse(content);
    if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
    if (typeof value.ok !== "boolean") return null;
    return {
      ok: value.ok,
      output: typeof value.output === "string" ? value.output : null,
      error: typeof value.error === "string" ? value.error : null,
      error_code: typeof value.error_code === "string" ? value.error_code : null,
    };
  } catch {
    return null;
  }
};

// ---------------------------------------------------------------------------
// Budget tiers and policies
// ---------------------------------------------------------------------------

export type ContextBudgetKind = "short" | "medium" | "large";

export interface CompactionPolicy {
  kind: ContextBudgetKind;
  /** Deterministic token budget the tier targets. */
  targetTokens: number;
  /** Successful read pairs kept per path (older reads are stale). */
  readsPerPath: number;
  /** Recent window of pairs never aged out (reads/explore). */
  keepRecentPairs: number;
  /** Drop successful find/search/browser pairs outside the recent window. */
  dropOldExplore: boolean;
  /** Analysis outside the last N turns is dropped locally. */
  analysisWindow: number;
}

export const COMPACTION_POLICIES: Readonly<Record<ContextBudgetKind, CompactionPolicy>> = {
  short: {
    kind: "short",
    targetTokens: 4_000,
    readsPerPath: 1,
    keepRecentPairs: 4,
    dropOldExplore: true,
    analysisWindow: 6,
  },
  medium: {
    kind: "medium",
    targetTokens: 8_000,
    readsPerPath: 1,
    keepRecentPairs: 8,
    dropOldExplore: true,
    analysisWindow: 10,
  },
  large: {
    kind: "large",
    targetTokens: 16_000,
    readsPerPath: 2,
    keepRecentPairs: Number.MAX_SAFE_INTEGER,
    dropOldExplore: false,
    analysisWindow: 48,
  },
};

/** Reads the tier for a budget argument (kind or explicit token count). */
export const policyForBudget = (budget: ContextBudgetKind): CompactionPolicy => COMPACTION_POLICIES[budget];

// ---------------------------------------------------------------------------
// Pair extraction
// ---------------------------------------------------------------------------

interface ConversationPair {
  assistantIndex: number;
  resultIndex: number;
  callId: string;
  tool: string;
  args: Record<string, unknown>;
  parsed: ParsedToolResult | null;
}

const isGuardPair = (pair: ConversationPair): boolean =>
  pair.parsed !== null &&
  (pair.parsed.error_code === "duplicate_call" || pair.parsed.error_code === "repeated_failure");

const isErrorPair = (pair: ConversationPair): boolean =>
  pair.parsed !== null ? !pair.parsed.ok || isGuardPair(pair) : true;

/** Extracts (assistant tool_call, tool result) pairs from a conversation. */
export function extractPairs(conversation: Conversation): readonly ConversationPair[] {
  const pairs: ConversationPair[] = [];
  const turns = conversation.turns;
  for (let i = 0; i < turns.length; i++) {
    const turn = turns[i];
    if (turn.role !== "assistant" || turn.toolCalls.length !== 1) continue;
    const call = turn.toolCalls[0];
    const next = turns[i + 1];
    if (next === undefined || next.role !== "tool" || next.toolCallId !== call.id) continue;
    pairs.push({
      assistantIndex: i,
      resultIndex: i + 1,
      callId: call.id,
      tool: call.name,
      args: parseStoredArguments(call.arguments),
      parsed: parseToolResultContent(next.content),
    });
  }
  return pairs;
}

function parseStoredArguments(argumentsText: string): Record<string, unknown> {
  try {
    const value = JSON.parse(argumentsText);
    if (typeof value === "object" && value !== null && !Array.isArray(value)) return value as Record<string, unknown>;
  } catch {
    // fall through
  }
  return {};
}

export type CompactionDropKind =
  | "stale_read"
  | "old_explore"
  | "analysis";

export interface CompactionDrop {
  kind: CompactionDropKind;
  detail: string;
}

export interface CompactionResult {
  conversation: Conversation;
  drops: readonly CompactionDrop[];
  estimatedTokens: number;
  metBudget: boolean;
  droppedTurnCount: number;
}

export interface CompactOptions {
  budget: ContextBudgetKind;
  /** Kept in the compaction tests; harness defaults to the tier policy. */
  policy?: CompactionPolicy;
}

/**
 * Compacts the model-facing view of one conversation for a budget tier.
 * The returned conversation preserves the contract-critical pairs; dropped
 * pairs are whole (assistant call + tool result) and never reordered.
 */
export function compactTranscript(conversation: Conversation, opts: CompactOptions): CompactionResult {
  const policy = opts.policy ?? policyForBudget(opts.budget);
  const pairs = extractPairs(conversation);
  const drops: CompactionDrop[] = [];

  // --- Phase 1: decide pair drops (never errors/patches/plan/shell/guards).
  const droppedPairIndexes = new Set<number>();
  const tailStart = Math.max(0, pairs.length - policy.keepRecentPairs);
  const lastReadIndexByPath = new Map<string, number[]>();
  pairs.forEach((pair, index) => {
    const path = typeof pair.args.path === "string" ? pair.args.path : null;
    if (pair.tool === "filesystem.read" && path !== null) {
      const list = lastReadIndexByPath.get(path) ?? [];
      list.push(index);
      lastReadIndexByPath.set(path, list);
    }
  });
  pairs.forEach((pair, index) => {
    if (isErrorPair(pair)) return;
    if (pair.tool === "editor.apply_patch" || pair.tool === "task.update_plan" || pair.tool === "shell.exec") return;
    if (index >= tailStart) return;
    const path = typeof pair.args.path === "string" ? pair.args.path : null;
    if (pair.tool === "filesystem.read" && path !== null) {
      const indexes = lastReadIndexByPath.get(path) ?? [];
      const rankFromEnd = indexes.length - 1 - indexes.indexOf(index);
      if (rankFromEnd >= policy.readsPerPath) {
        droppedPairIndexes.add(index);
        drops.push({ kind: "stale_read", detail: `${pair.tool} ${path}` });
      }
      return;
    }
    if (policy.dropOldExplore) {
      droppedPairIndexes.add(index);
      drops.push({ kind: "old_explore", detail: `${pair.tool} ${JSON.stringify(pair.args)}` });
    }
  });

  // --- Phase 2: rebuild turns (drop whole pairs) and apply analysis window.
  const turns = conversation.turns;
  const kept: ConversationTurn[] = [];
  let droppedTurns = 0;
  for (let i = 0; i < turns.length; i++) {
    const inDroppedPair = pairs.some((pair, pi) =>
      droppedPairIndexes.has(pi) &&
      (pair.assistantIndex === i || pair.resultIndex === i)
    );
    if (inDroppedPair) {
      droppedTurns += 1;
      continue;
    }
    const turn = turns[i];
    if (turn.role === "assistant" && turn.analysis.length > 0 && i < turns.length - policy.analysisWindow) {
      kept.push({ ...turn, analysis: [] as const });
      drops.push({ kind: "analysis", detail: `assistant turn ${i}` });
      continue;
    }
    kept.push(turn);
  }
  const compacted: Conversation = { turns: kept };

  const estimatedTokens = estimateConversationTokens(compacted);
  return {
    conversation: compacted,
    drops,
    estimatedTokens,
    metBudget: estimatedTokens <= policy.targetTokens,
    droppedTurnCount: droppedTurns,
  };
}

// ---------------------------------------------------------------------------
// Structured context rendering
// ---------------------------------------------------------------------------

export interface StructuredContextOptions {
  /** Recent transcript turns appended after the state summary. */
  maxTailTurns?: number;
}

const isSystemish = (turn: ConversationTurn): boolean => turn.role === "system" || turn.role === "developer";

/**
 * Renders the structured task context: the deterministic state summary plus
 * the recent transcript tail.  This is the model-facing replacement for a
 * full transcript replay at any budget tier.
 */
export function renderStructuredContext(
  state: StructuredTaskState,
  conversation: Conversation,
  opts: StructuredContextOptions = {},
): string {
  const tail: ConversationTurn[] = [];
  for (let i = conversation.turns.length - 1; i >= 0 && tail.length < (opts.maxTailTurns ?? 6); i--) {
    const turn = conversation.turns[i];
    if (isSystemish(turn)) continue;
    tail.unshift(turn);
  }
  const body = [
    "[structured task state]",
    renderStateSummary(state),
    "",
    "[recent transcript]",
    JSON.stringify(wireMessagesFromConversation({ turns: tail })),
  ].join("\n");
  return body;
}

export const estimateStructuredContextTokens = (text: string): number => estimateTokens(text);
