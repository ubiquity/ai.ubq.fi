/**
 * Gateway Jev compaction route, adapted from 0x4007/fast-jev-compaction (MIT),
 * commit c1eab5fdbd6bde7d67f2da070116496558836884, `codex/jev-compaction-proxy.ts`:
 * the Jev asker, per-call timeout transport, response payloads and fail-closed
 * validation. Mechanical changes: Deno `.ts` specifiers, repository formatting,
 * a parsed body instead of a raw string, caller abort propagation into the Jev
 * call, gateway `openaiError` responses, and no HTTP server or CLI entrypoint.
 * Selection and rendering stay in the ported library; this file only orchestrates
 * and validates. See ./LICENSE and ./README.md.
 *
 * Contract: only a request whose `x-codex-turn-metadata` marks
 * `request_kind: "compaction"` with `compaction.implementation: "responses"` is
 * handled here. Every failure returns a non-success response before any
 * successful completion is emitted, so Codex keeps its existing history; kept
 * content is never truncated to force a success and no usage is invented.
 */
import { openaiError } from "../http.ts";
import { setResponseCompletionTelemetry } from "../openai.ts";
import { readJsonBody } from "../request.ts";
import { JevClient } from "../../lib/jev_compaction/client.ts";
import { compact } from "../../lib/jev_compaction/compact.ts";
import { isResponsesCompaction, MAX_SUMMARY_CHARS, parseCodexInput, parseTurnMetadata, renderSummary } from "../../lib/jev_compaction/codex_items.ts";
import { collectToolCalls } from "../../lib/jev_compaction/state.ts";
import type { CompactResult, JevAsker, ToolCall } from "../../lib/jev_compaction/types.ts";

const TURN_METADATA_HEADER = "x-codex-turn-metadata";

/** Library defaults, minus the goal (the transcript itself carries the task). */
export const COMPACTION_OPTIONS = {
  preserveRecentMessages: 6,
  keepThreshold: 0.5,
  maxStateTokens: 25_000,
  maxRequestTokens: 30_000,
  truncateHeadChars: 300,
} as const;

/** Bound on one Jev HTTP call, headers and response body included. */
export const JEV_TIMEOUT_MS = 30_000;

type FailureKind =
  | "unparsable-body"
  | "missing-input"
  | "empty-transcript"
  | "no-candidates"
  | "missing-key"
  | "jev-failed"
  | "no-reduction"
  | "render-failed"
  | "summary-too-large"
  | "empty-summary";

export class CompactionUnavailable extends Error {
  readonly kind: FailureKind;

  constructor(kind: FailureKind, detail: string) {
    super(detail);
    this.name = "CompactionUnavailable";
    this.kind = kind;
  }
}

export type CompactionOutcome = {
  status: number;
  contentType: string;
  headers: Record<string, string>;
  body: string;
};

function record(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : null;
}

/** Header-only predicate; absent, malformed or unknown markers stay on the normal route. */
export function isJevCompactionRequest(req: Request): boolean {
  return isResponsesCompaction(parseTurnMetadata(req.headers.get(TURN_METADATA_HEADER)));
}

function ssePayload(itemId: string, responseId: string, text: string): string {
  const item = {
    type: "message",
    role: "assistant",
    id: itemId,
    content: [{ type: "output_text", text }],
  };
  const done = JSON.stringify({ type: "response.output_item.done", item });
  const completed = JSON.stringify({
    type: "response.completed",
    response: { id: responseId, status: "completed" },
  });
  return `event: response.output_item.done\ndata: ${done}\n\nevent: response.completed\ndata: ${completed}\n\n`;
}

function jsonCompletionPayload(itemId: string, responseId: string, text: string): string {
  return JSON.stringify({
    id: responseId,
    object: "response",
    status: "completed",
    output: [
      {
        type: "message",
        role: "assistant",
        id: itemId,
        content: [{ type: "output_text", text }],
      },
    ],
  });
}

/** Structural only: counts and sizes, never transcript text. */
function structuralHeader(counts: {
  kept: number;
  resultsDropped: number;
  callsDropped: number;
  pinned: number;
  charsBefore: number;
  charsAfter: number;
}): string {
  return [
    "summary",
    `kept=${counts.kept}`,
    `results_dropped=${counts.resultsDropped}`,
    `calls_dropped=${counts.callsDropped}`,
    `pinned=${counts.pinned}`,
    `chars_before=${counts.charsBefore}`,
    `chars_after=${counts.charsAfter}`,
  ].join("; ");
}

/**
 * Nonsecret failure class for logs: the failure kind plus a bounded structural
 * token. Library error messages can embed provider response text, so only
 * status codes, counts, and fixed classifications are ever logged.
 */
function failureLogDetail(kind: FailureKind, message: string): string {
  const http = /Jev request failed \((\d{3})\)/.exec(message);
  if (http) return `http=${http[1]}`;
  if (message.startsWith("Jev request timed out")) return "timeout";
  if (message.startsWith("Jev request aborted")) return "aborted";
  if (message.includes("malformed JSON")) return "malformed-response";
  if (message.startsWith("Invalid Jev answer")) return "invalid-answer";
  if (message.includes("no room for questions")) return "state-over-budget";
  if (message.includes("history too large for Jev")) return "state-too-large";
  if (message.includes("TYPESAFE_API_KEY")) return "missing-key";
  const counts = /dropped=(\d+), before=(\d+), after=(\d+)/.exec(message);
  if (counts) return `dropped=${counts[1]} before=${counts[2]} after=${counts[3]}`;
  const chars = /summary is (\d+) chars/.exec(message);
  if (chars) return `summary-chars=${chars[1]}`;
  return kind;
}

function logCompaction(line: Record<string, unknown>): void {
  console.info("[ai.ubq.fi] jev_compaction", JSON.stringify(line));
}

function assertEveryCandidateDecided(result: CompactResult, candidates: readonly ToolCall[]): void {
  const decidedIds = new Set(result.decisions.map((decision) => decision.id));
  for (const candidate of candidates) {
    if (!decidedIds.has(candidate.id)) {
      throw new CompactionUnavailable("jev-failed", `missing decision for ${candidate.id}`);
    }
  }
}

function assertCompactionReduced(result: CompactResult): void {
  const dropped = result.stats.resultsDropped + result.stats.callsDropped;
  if (dropped === 0 || result.stats.charsAfter >= result.stats.charsBefore) {
    throw new CompactionUnavailable(
      "no-reduction",
      `nothing was dropped (dropped=${dropped}, before=${result.stats.charsBefore}, after=${result.stats.charsAfter})`
    );
  }
}

function renderCompactedSummary(transcript: ReturnType<typeof parseCodexInput>, calls: readonly ToolCall[], result: CompactResult): string {
  if (transcript === null) throw new CompactionUnavailable("missing-input", "compaction request has no input items array");
  const callIds = new Map(calls.map((call) => [call.id, call.tool_use_id]));
  let summary: string;
  try {
    summary = renderSummary(transcript, {
      decisions: result.decisions,
      callIds,
      headChars: COMPACTION_OPTIONS.truncateHeadChars,
      stats: result.stats,
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new CompactionUnavailable("render-failed", `render failed: ${detail.slice(0, 200)}`);
  }
  if (summary.trim().length === 0) {
    throw new CompactionUnavailable("empty-summary", "renderer produced an empty summary");
  }
  if (summary.length > MAX_SUMMARY_CHARS) {
    // Never truncate Jev-kept content silently: fail and keep Codex's history.
    throw new CompactionUnavailable("summary-too-large", `summary is ${summary.length} chars (limit ${MAX_SUMMARY_CHARS})`);
  }
  return summary;
}

/**
 * Turns one header-verified compaction request body into a completed response.
 * Throws `CompactionUnavailable` for every non-success path; the caller maps it
 * to an explicit error status so Codex keeps the original history.
 */
export async function buildCompactionResponse(body: unknown, asker: JevAsker, options: { stream: boolean }): Promise<CompactionOutcome> {
  const requestBody: Record<string, unknown> = record(body) ?? {};
  const transcript = parseCodexInput(requestBody.input);
  if (!transcript) {
    throw new CompactionUnavailable("missing-input", "compaction request has no input items array");
  }
  if (transcript.entries.length === 0) {
    throw new CompactionUnavailable("empty-transcript", "compaction request has no conversation content");
  }
  const calls = collectToolCalls(transcript.messages, COMPACTION_OPTIONS.preserveRecentMessages);
  const candidates = calls.filter((call) => !call.pinned);
  if (candidates.length === 0) {
    // A short or tool-free transcript has nothing Jev could usefully decide.
    // No Jev request is made and no summary is invented.
    throw new CompactionUnavailable("no-candidates", "no unpinned tool call/result pairs to decide");
  }

  let result: CompactResult;
  try {
    result = await compact(transcript.messages, asker, COMPACTION_OPTIONS);
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    // The library validates every asked question, so a thrown error here means
    // no partial decision set was substituted.
    throw new CompactionUnavailable("jev-failed", detail.slice(0, 200));
  }

  assertEveryCandidateDecided(result, candidates);
  assertCompactionReduced(result);
  const summary = renderCompactedSummary(transcript, calls, result);

  logCompaction({
    outcome: "ok",
    items: transcript.entries.length,
    candidates: candidates.length,
    kept: result.stats.kept,
    results_dropped: result.stats.resultsDropped,
    calls_dropped: result.stats.callsDropped,
    pinned: result.stats.pinned,
    chars_before: result.stats.charsBefore,
    chars_after: result.stats.charsAfter,
    jev_requests: result.stats.requests,
    state_stage: result.stats.stateStage || "none",
    total_ms: result.stats.ms,
  });

  const suffix = crypto.randomUUID().replaceAll("-", "").slice(0, 16);
  const headers = {
    "x-jev-compaction": structuralHeader(result.stats),
  };
  if (options.stream) {
    return {
      status: 200,
      contentType: "text/event-stream",
      headers,
      body: ssePayload(`msg_jev_${suffix}`, `resp_jev_${suffix}`, summary),
    };
  }
  return {
    status: 200,
    contentType: "application/json",
    headers,
    body: jsonCompletionPayload(`msg_jev_${suffix}`, `resp_jev_${suffix}`, summary),
  };
}

function timedOutError(timeoutMs: number): Error {
  return new Error(`Jev request timed out after ${timeoutMs}ms`);
}

/**
 * Adapter-owned Jev transport. One request is bounded by an `AbortSignal`
 * (30 s by default) that stays armed while the response body is consumed, any
 * caller signal is preserved, and the timer and listeners are released on
 * every path. Transport failures are re-thrown with a fixed, non-secret
 * message.
 */
export function createJevTimeoutFetch(baseFetch: typeof fetch, timeoutMs = JEV_TIMEOUT_MS): typeof fetch {
  return (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const controller = new AbortController();
    const external = init?.signal ?? null;
    const forwardExternalAbort = (): void => {
      controller.abort(external?.reason);
    };
    if (external) {
      if (external.aborted) controller.abort(external.reason);
      else external.addEventListener("abort", forwardExternalAbort, { once: true });
    }
    const timer = setTimeout(() => {
      controller.abort(timedOutError(timeoutMs));
    }, timeoutMs);
    let released = false;
    const release = (): void => {
      if (released) return;
      released = true;
      clearTimeout(timer);
      external?.removeEventListener("abort", forwardExternalAbort);
    };
    const sanitize = (error: unknown): Error => {
      const reason: unknown = controller.signal.reason;
      if (reason instanceof Error && reason.message.startsWith("Jev request timed out")) return reason;
      if (controller.signal.aborted) return new Error("Jev request aborted");
      const detail = error instanceof Error ? error.message : "unknown transport failure";
      return new Error(`Jev transport error: ${detail.slice(0, 120)}`);
    };
    const swallowCancellation = (): void => {
      return;
    };
    return baseFetch(input, { ...init, signal: controller.signal }).then(
      (response) => {
        const body = response.body;
        if (!body) {
          release();
          return response;
        }
        const reader = body.getReader();
        const cancelReader = (reason?: unknown): Promise<void> => reader.cancel(reason).catch(swallowCancellation);
        let aborted = false;
        controller.signal.addEventListener(
          "abort",
          () => {
            aborted = true;
            void cancelReader(controller.signal.reason);
          },
          { once: true }
        );
        const stream = new ReadableStream<Uint8Array>({
          async pull(streamController) {
            try {
              const { done, value } = await reader.read();
              if (done) {
                release();
                // An abort that landed after the last chunk still fails the read.
                if (aborted) streamController.error(sanitize(controller.signal.reason));
                else streamController.close();
                return;
              }
              streamController.enqueue(value);
            } catch (error) {
              release();
              streamController.error(sanitize(error));
            }
          },
          cancel(reason) {
            release();
            return cancelReader(reason);
          },
        });
        return new Response(stream, {
          status: response.status,
          statusText: response.statusText,
          headers: response.headers,
        });
      },
      (error: unknown) => {
        release();
        throw sanitize(error);
      }
    );
  };
}

/**
 * Safe lazy credential access: read only when a recognized compaction request
 * needs it, and treat a denied or absent value as an explicit failure instead of
 * a process-startup failure. Never logged or echoed.
 */
function typesafeApiKey(): string | null {
  try {
    const key = Deno.env.get("TYPESAFE_API_KEY");
    return key !== undefined && key.length > 0 ? key : null;
  } catch {
    return null;
  }
}

export type JevCompactionDeps = {
  /** Caller-owned signal; a client cancellation stops the Jev calls and the operation. */
  signal?: AbortSignal;
  /** Test seam: replaces the HTTP asker entirely. */
  asker?: JevAsker;
  /** Test seam: replaces the Jev transport fetch. */
  fetch?: typeof fetch;
  /** Resolved credential for this call; `null` forces the explicit missing-key failure. */
  apiKey?: string | null;
};

let askerForTest: JevAsker | null = null;

/** Test seam: route every compaction through a fake asker; `null` restores the real one. */
export const setJevCompactionAskerForTest = (asker: JevAsker | null): void => {
  askerForTest = asker;
};

function defaultAsker(deps: JevCompactionDeps): JevAsker {
  const apiKey = deps.apiKey === undefined ? typesafeApiKey() : deps.apiKey;
  if (!apiKey) throw new CompactionUnavailable("missing-key", "TYPESAFE_API_KEY is not configured");
  return new JevClient({
    apiKey,
    fetch: createJevTimeoutFetch(deps.fetch ?? fetch, JEV_TIMEOUT_MS),
    ...(deps.signal ? { signal: deps.signal } : {}),
  });
}

function cancelledResponse(): Response {
  return openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null });
}

function failureResponse(kind: FailureKind): Response {
  const message = `Jev compaction unavailable (${kind})`;
  if (kind === "missing-key") {
    return openaiError(503, message, "jev_compaction_unavailable", { type: "server_error" });
  }
  if (kind === "jev-failed" || kind === "render-failed") {
    return openaiError(502, message, "jev_compaction_unavailable", { type: "server_error" });
  }
  return openaiError(400, message, "jev_compaction_unavailable");
}

/**
 * Handles one recognized compaction request end to end. The body is read only
 * here (the normal route is never parsed), Jev work is bounded by the caller
 * signal and the per-call timeout, and every failure is a non-success response.
 */
export async function handleJevResponsesCompaction(req: Request, deps: JevCompactionDeps = {}): Promise<Response> {
  if (deps.signal?.aborted) return cancelledResponse();
  const body = await readJsonBody(req);
  if (body === null) {
    logCompaction({ outcome: "failed", kind: "unparsable-body" });
    return failureResponse("unparsable-body");
  }
  const stream = record(body)?.stream !== false;
  let asker: JevAsker;
  try {
    asker = deps.asker ?? askerForTest ?? defaultAsker(deps);
  } catch (error) {
    const kind = error instanceof CompactionUnavailable ? error.kind : "jev-failed";
    logCompaction({ outcome: "failed", kind, detail: failureLogDetail(kind, error instanceof Error ? error.message : "") });
    return failureResponse(kind);
  }
  let outcome: CompactionOutcome;
  try {
    outcome = await buildCompactionResponse(body, asker, { stream });
  } catch (error) {
    if (deps.signal?.aborted) {
      logCompaction({ outcome: "failed", kind: "cancelled" });
      return cancelledResponse();
    }
    const kind = error instanceof CompactionUnavailable ? error.kind : "jev-failed";
    const detail = error instanceof Error ? error.message : String(error);
    logCompaction({ outcome: "failed", kind, detail: failureLogDetail(kind, detail) });
    return failureResponse(kind);
  }
  // Completion telemetry goes on the success path only: the terminal wrapper's
  // normal completion decision then detects this local answer and settles the
  // admitted request as completed, instead of releasing it as a stream that
  // ended without a completion. Failures above return unmarked responses and
  // still release through the same wrapper.
  return setResponseCompletionTelemetry(
    new Response(outcome.body, {
      status: outcome.status,
      headers: { "content-type": outcome.contentType, "cache-control": "no-store", ...outcome.headers },
    }),
    stream
  );
}
