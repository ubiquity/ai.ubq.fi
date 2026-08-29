/**
 * Harmony/Cerebras protocol probe manifest (plan m01).
 *
 * One bounded, staged, deterministic reproduction per protocol question:
 * reasoning return/replay, tool call/result shape, mixed strictness,
 * structured output with tools, reasoning efforts, parallel calls,
 * consecutive tool turns, and native Harmony versus generic calls.
 *
 * Every probe renders a sanitized record: request metadata (roles, tool
 * names, strictness values, flags) and response metadata (status, presence
 * and length of content/reasoning, tool-call shape, a short final-content
 * preview).  Prompts, private reasoning, tool argument values, API keys and
 * raw bodies never appear in results.  Full normalized responses stay in
 * memory only (via {@link ProbeTurnRun}) and are never serialized.
 *
 * Live runs reuse the existing `CEREBRAS_API_KEY`; deterministic tests drive
 * the same manifest through fake transports.
 */

import {
  buildCerebrasHarmonyRequest,
  type HarmonyRequestOptions,
  type HarmonyTransport,
  normalizeHarmonyChatCompletion,
  runHarmonyTurn,
} from "./adapter.ts";
import {
  type BootstrapClassifierRequestOptions,
  type BootstrapClassifierVerdict,
  buildBootstrapClassifierRequest,
  verdictFromBootstrapResponse,
} from "./classifier.ts";
import {
  advanceConversation,
  analysisLineCount,
  appendToolResult,
  appendUser,
  type Conversation,
  createConversation,
  dropAnalysisBeforeCompletedFinal,
  hasCompletedFinal,
  pendingToolCallCount,
} from "./conversation.ts";
import type { HarmonyCallStyle, NormalizedAssistantResponse, ToolDefinition } from "./types.ts";
import { HARMONY_CEREBRAS_MODEL } from "./types.ts";

// ---------------------------------------------------------------------------
// Sanitized record shapes
// ---------------------------------------------------------------------------

export type ProbeGroup =
  | "reasoning"
  | "replay"
  | "tools"
  | "strictness"
  | "structured"
  | "parallel"
  | "classifier";

export type ProbeOutcome = "ok" | "upstream_rejected" | "upstream_error" | "adapter_error" | "failed";

export type ProbeRequestSummary = Readonly<{
  style: HarmonyCallStyle | "classifier";
  model: string;
  roles: readonly string[];
  tools: ReadonlyArray<{ name: string; strict: boolean | null }> | null;
  toolStrictnessValues: readonly boolean[];
  reasoningEffortTopLevel: string | null;
  reasoningInSystem: boolean;
  responseFormat: "json_object" | "json_schema" | "developer" | "none";
  parallelToolCalls: boolean | null;
  maxCompletionTokens: number | null;
  analysisInWire: boolean;
  assistantToolTurns: number;
  toolResultTurns: number;
}>;

export type ProbeResponseSummary = Readonly<{
  status: number;
  model: string;
  contentPresent: boolean;
  contentChars: number;
  contentPreview: string | null;
  reasoningPresent: boolean;
  reasoningChars: number;
  toolCalls: ReadonlyArray<{ id: string; name: string; argumentsChars: number; argumentsJsonValid: boolean }>;
  refusal: boolean;
  finishReason: string | null;
}>;

export type ProbeStateSnapshot = Readonly<{
  analysisLines: number;
  analysisAfterDrop: number;
  pendingToolCalls: number;
  completedFinal: boolean;
}>;

export type ProbeTurnRecord = Readonly<{
  outcome: ProbeOutcome;
  status: number | null;
  durationMs: number;
  request: ProbeRequestSummary | null;
  response: ProbeResponseSummary | null;
  upstreamError: Readonly<{ code: string | null; message: string | null }> | null;
  adapterError: Readonly<{ code: string; message: string }> | null;
  state: ProbeStateSnapshot | null;
  notes: readonly string[];
  verdict: BootstrapClassifierVerdict | null;
}>;

/**
 * One executed model turn.  `record` is serializable; `normalized` and
 * `verdict` are scenario-local state and must never be written to output.
 */
export type ProbeTurnRun = Readonly<{
  record: ProbeTurnRecord;
  normalized: NormalizedAssistantResponse | null;
  verdict: BootstrapClassifierVerdict | null;
}>;

export type ProbeScenarioResult = Readonly<{
  id: string;
  group: ProbeGroup;
  style: HarmonyCallStyle | "classifier";
  description: string;
  expectedOutcome: ProbeOutcome | null;
  outcome: ProbeOutcome;
  startedAt: string;
  durationMs: number;
  turns: readonly ProbeTurnRecord[];
  verdict: BootstrapClassifierVerdict | null;
  failure: string | null;
  notes: readonly string[];
}>;

// ---------------------------------------------------------------------------
// Shared probe fixtures
// ---------------------------------------------------------------------------

export const WEATHER_TOOL: ToolDefinition = {
  name: "get_weather",
  description: "Gets the current weather in the provided location.",
  parameters: {
    type: "object",
    properties: { location: { type: "string", description: "The city and state, e.g. San Francisco, CA" } },
    required: ["location"],
    additionalProperties: false,
  },
};

export const NOTE_TOOL: ToolDefinition = {
  name: "save_note",
  description: "Saves a short note to the conversation log.",
  parameters: {
    type: "object",
    properties: { text: { type: "string", description: "Note text" } },
    required: ["text"],
    additionalProperties: false,
  },
};

export const ANSWER_SCHEMA: Record<string, unknown> = {
  type: "object",
  properties: { answer: { type: "integer", description: "The computed answer" } },
  required: ["answer"],
  additionalProperties: false,
};

const user = (content: string): Conversation => createConversation([{ role: "user", content }]);

const argsJsonValid = (argumentsText: string): boolean => {
  try {
    JSON.parse(argumentsText);
    return true;
  } catch {
    return false;
  }
};

// ---------------------------------------------------------------------------
// Probe context: sanitized turn runner
// ---------------------------------------------------------------------------

export type ProbeContext = Readonly<{
  transport: HarmonyTransport;
  now: () => Date;
  runTurn: (conversation: Conversation, options: Omit<HarmonyRequestOptions, "turns">) => Promise<ProbeTurnRun>;
  runRaw: (body: Record<string, unknown>, conversation?: Conversation) => Promise<ProbeTurnRun>;
  runClassifier: (options: BootstrapClassifierRequestOptions) => Promise<ProbeTurnRun>;
}>;

const stateSnapshot = (conversation: Conversation | undefined): ProbeStateSnapshot | null => {
  if (!conversation) return null;
  const dropped = dropAnalysisBeforeCompletedFinal(conversation);
  return {
    analysisLines: analysisLineCount(conversation),
    analysisAfterDrop: analysisLineCount(dropped),
    pendingToolCalls: pendingToolCallCount(conversation),
    completedFinal: hasCompletedFinal(conversation),
  };
};

const summarizeRawBody = (body: Record<string, unknown>): ProbeRequestSummary => {
  const toolsValue = body.tools;
  const tools: { name: string; strict: boolean | null }[] = [];
  if (Array.isArray(toolsValue)) {
    for (const tool of toolsValue) {
      if (!tool || typeof tool !== "object") continue;
      const fn = (tool as Record<string, unknown>).function;
      if (!fn || typeof fn !== "object") continue;
      const functionRecord = fn as Record<string, unknown>;
      const name = typeof functionRecord.name === "string" ? functionRecord.name : "?";
      const strict = typeof functionRecord.strict === "boolean" ? functionRecord.strict : null;
      tools.push({ name, strict });
    }
  }
  const messages = Array.isArray(body.messages) ? body.messages : [];
  const roles = messages.map((message) =>
    message && typeof message === "object" ? String((message as Record<string, unknown>).role ?? "?") : "?"
  );
  const format = body.response_format;
  const formatType = format && typeof format === "object" ? (format as Record<string, unknown>).type : undefined;
  const responseFormat = formatType === "json_object"
    ? "json_object"
    : formatType === "json_schema"
    ? "json_schema"
    : "none";
  return {
    style: "generic",
    model: typeof body.model === "string" ? body.model : "?",
    roles,
    tools: tools.length > 0 ? tools : null,
    toolStrictnessValues: tools.map((tool) => tool.strict ?? false),
    reasoningEffortTopLevel: typeof body.reasoning_effort === "string" ? body.reasoning_effort : null,
    reasoningInSystem: false,
    responseFormat,
    parallelToolCalls: typeof body.parallel_tool_calls === "boolean" ? body.parallel_tool_calls : null,
    maxCompletionTokens: typeof body.max_completion_tokens === "number" ? body.max_completion_tokens : null,
    analysisInWire: roles.includes("assistant"),
    assistantToolTurns:
      messages.filter((message) =>
        message && typeof message === "object" && Array.isArray((message as Record<string, unknown>).tool_calls)
      ).length,
    toolResultTurns: roles.filter((role) => role === "tool").length,
  };
};

const responseSummary = (status: number, normalized: NormalizedAssistantResponse): ProbeResponseSummary => ({
  status,
  model: normalized.model,
  contentPresent: normalized.content !== null,
  contentChars: normalized.content?.length ?? 0,
  contentPreview: normalized.content ? normalized.content.slice(0, 120) : null,
  reasoningPresent: normalized.analysis.length > 0,
  reasoningChars: normalized.shape.reasoningChars,
  toolCalls: normalized.toolCalls.map((call) => ({
    id: call.id,
    name: call.name,
    argumentsChars: call.arguments.length,
    argumentsJsonValid: argsJsonValid(call.arguments),
  })),
  refusal: normalized.refusal !== null,
  finishReason: normalized.finishReason,
});

const outcomeForStatus = (status: number): ProbeOutcome =>
  status >= 400 && status < 500 ? "upstream_rejected" : "upstream_error";

const upstreamErrorOf = (value: unknown): { code: string | null; message: string | null } | null => {
  const inner = value && typeof value === "object" ? (value as Record<string, unknown>) : {};
  return {
    code: typeof inner.code === "string" ? inner.code : null,
    message: typeof inner.message === "string" ? inner.message : null,
  };
};

export const createProbeContext = (
  transport: HarmonyTransport,
  now: () => Date = () => new Date(),
): ProbeContext => {
  const record = (
    outcome: ProbeOutcome,
    status: number | null,
    durationMs: number,
    request: ProbeRequestSummary | null,
    response: ProbeResponseSummary | null,
    upstreamError: { code: string | null; message: string | null } | null,
    adapterError: { code: string; message: string } | null,
    conversation: Conversation | undefined,
    notes: readonly string[] = [],
    verdict: BootstrapClassifierVerdict | null = null,
  ): ProbeTurnRecord => ({
    outcome,
    status,
    durationMs,
    request,
    response,
    upstreamError,
    adapterError,
    state: stateSnapshot(conversation),
    notes,
    verdict,
  });

  return {
    transport,
    now,
    runTurn: async (conversation, options) => {
      const started = now().getTime();
      try {
        const built = buildCerebrasHarmonyRequest({ ...options, turns: conversation.turns });
        const request: ProbeRequestSummary = {
          style: built.style,
          model: built.metadata.model,
          roles: built.metadata.messageRoles,
          tools: built.metadata.toolEntries.length > 0 ? built.metadata.toolEntries : null,
          toolStrictnessValues: built.metadata.toolStrictnessValues,
          reasoningEffortTopLevel: built.metadata.reasoningEffortTopLevel,
          reasoningInSystem: built.metadata.reasoningEffortInSystem,
          responseFormat: built.metadata.responseFormat,
          parallelToolCalls: built.metadata.parallelToolCalls,
          maxCompletionTokens: built.metadata.maxCompletionTokens,
          analysisInWire: built.metadata.analysisInWire,
          assistantToolTurns: built.metadata.assistantToolTurns,
          toolResultTurns: built.metadata.toolResultTurns,
        };
        const result = await runHarmonyTurn({ ...options, turns: conversation.turns }, transport);
        const durationMs = now().getTime() - started;
        if (result.ok) {
          return {
            record: record(
              "ok",
              result.status,
              durationMs,
              request,
              responseSummary(result.status, result.normalized),
              null,
              null,
              conversation,
            ),
            normalized: result.normalized,
            verdict: null,
          };
        }
        if (result.upstreamError) {
          return {
            record: record(
              outcomeForStatus(result.status),
              result.status,
              durationMs,
              request,
              null,
              result.upstreamError,
              null,
              conversation,
            ),
            normalized: null,
            verdict: null,
          };
        }
        return {
          record: record("failed", result.status, durationMs, request, null, null, {
            code: "normalization_error",
            message: result.normalizationError ?? "unknown",
          }, conversation),
          normalized: null,
          verdict: null,
        };
      } catch (error) {
        const durationMs = now().getTime() - started;
        if (error instanceof Error && "code" in error) {
          return {
            record: record("adapter_error", null, durationMs, null, null, null, {
              code: String((error as { code: unknown }).code),
              message: error.message,
            }, conversation),
            normalized: null,
            verdict: null,
          };
        }
        return {
          record: record(
            "failed",
            null,
            durationMs,
            null,
            null,
            null,
            { code: "probe_error", message: String(error) },
            conversation,
          ),
          normalized: null,
          verdict: null,
        };
      }
    },
    runRaw: async (body, conversation) => {
      const started = now().getTime();
      const request = summarizeRawBody(body);
      try {
        const response = await transport(body);
        const status = response.status;
        if (!response.ok) {
          const value = await response.json().catch(() => null);
          return {
            record: record(
              outcomeForStatus(status),
              status,
              now().getTime() - started,
              request,
              null,
              upstreamErrorOf(value),
              null,
              conversation,
            ),
            normalized: null,
            verdict: null,
          };
        }
        const value = await response.json().catch(() => null);
        if (value === null) {
          return {
            record: record("failed", status, now().getTime() - started, request, null, null, {
              code: "non_json",
              message: "upstream reply is not JSON",
            }, conversation),
            normalized: null,
            verdict: null,
          };
        }
        const normalized = normalizeHarmonyChatCompletion(value);
        if ("error" in normalized) {
          return {
            record: record("failed", status, now().getTime() - started, request, null, null, {
              code: "normalization_error",
              message: normalized.error,
            }, conversation),
            normalized: null,
            verdict: null,
          };
        }
        return {
          record: record(
            "ok",
            status,
            now().getTime() - started,
            request,
            responseSummary(status, normalized),
            null,
            null,
            conversation,
          ),
          normalized,
          verdict: null,
        };
      } catch (error) {
        return {
          record: record("failed", null, now().getTime() - started, request, null, null, {
            code: "probe_error",
            message: String(error),
          }, conversation),
          normalized: null,
          verdict: null,
        };
      }
    },
    runClassifier: async (classifierOptions) => {
      const started = now().getTime();
      const body = buildBootstrapClassifierRequest(classifierOptions);
      const request: ProbeRequestSummary = {
        style: "classifier",
        model: typeof body.model === "string" ? body.model : "?",
        roles: ["system", "developer", "user"],
        tools: null,
        toolStrictnessValues: [],
        reasoningEffortTopLevel: null,
        reasoningInSystem: true,
        responseFormat: "none",
        parallelToolCalls: null,
        maxCompletionTokens: typeof body.max_completion_tokens === "number" ? body.max_completion_tokens : null,
        analysisInWire: false,
        assistantToolTurns: 0,
        toolResultTurns: 0,
      };
      try {
        const response = await transport(body);
        const status = response.status;
        if (!response.ok) {
          const value = await response.json().catch(() => null);
          const unknown = {
            verdict: "unknown",
            raw: null,
            reason: `classifier request rejected with status ${status}`,
          } as const;
          return {
            record: record(
              outcomeForStatus(status),
              status,
              now().getTime() - started,
              request,
              null,
              upstreamErrorOf(value),
              null,
              undefined,
              [`verdict=unknown`],
              unknown,
            ),
            normalized: null,
            verdict: unknown,
          };
        }
        const value = await response.json().catch(() => null);
        if (value === null) {
          const unknown = {
            verdict: "unknown",
            raw: null,
            reason: "classifier reply is not JSON",
          } as const;
          return {
            record: record(
              "failed",
              status,
              now().getTime() - started,
              request,
              null,
              null,
              {
                code: "non_json",
                message: "upstream reply is not JSON",
              },
              undefined,
              [`verdict=unknown`],
              unknown,
            ),
            normalized: null,
            verdict: unknown,
          };
        }
        const normalized = normalizeHarmonyChatCompletion(value);
        if ("error" in normalized) {
          const unknown = {
            verdict: "unknown",
            raw: null,
            reason: `classifier reply is invalid: ${normalized.error}`,
          } as const;
          return {
            record: record(
              "failed",
              status,
              now().getTime() - started,
              request,
              null,
              null,
              {
                code: "normalization_error",
                message: normalized.error,
              },
              undefined,
              [`verdict=unknown`],
              unknown,
            ),
            normalized: null,
            verdict: unknown,
          };
        }
        const verdict = verdictFromBootstrapResponse(normalized);
        return {
          record: record(
            "ok",
            status,
            now().getTime() - started,
            request,
            responseSummary(status, normalized),
            null,
            null,
            undefined,
            [`verdict=${verdict.verdict}`],
            verdict,
          ),
          normalized,
          verdict,
        };
      } catch (error) {
        const unknown = {
          verdict: "unknown",
          raw: null,
          reason: `classifier transport failed: ${String(error)}`,
        } as const;
        return {
          record: record(
            "failed",
            null,
            now().getTime() - started,
            request,
            null,
            null,
            {
              code: "probe_error",
              message: String(error),
            },
            undefined,
            [`verdict=unknown`],
            unknown,
          ),
          normalized: null,
          verdict: unknown,
        };
      }
    },
  };
};

// ---------------------------------------------------------------------------
// Scenario definitions
// ---------------------------------------------------------------------------

export type ProbeScenario = Readonly<{
  id: string;
  group: ProbeGroup;
  style: HarmonyCallStyle | "classifier";
  description: string;
  expectedOutcome: ProbeOutcome | null;
  run: (ctx: ProbeContext) => Promise<ProbeScenarioResult>;
}>;

const MAX_TOKENS = 256;
const WEATHER_QUESTION = "Call get_weather with location San Francisco, then report the weather in one sentence.";
const WEATHER_RESULT = '{"temperature": 20, "description": "sunny"}';

const CLASSIFIER_OBSERVATION = {
  runId: "probe-run-42",
  generation: 2,
  phase: "apply",
  milestone: "implementation-verified",
  failureFingerprint: "verification-failed-v1",
  gitSha: "abc1234def5678",
  ledgerVersion: 7,
  retryState: "retry-2",
  verificationEvidence: "verification command passed",
} as const;

const CLASSIFIER_DECISION = "Decide whether the run made material progress since the previous evaluation. " +
  "Return true only if new durable evidence is present: a later verified milestone, " +
  "a new accepted Git identity, or a monotonic ledger advance tied to useful work. " +
  "Return false when nothing durable changed or the run cycles without advancement. " +
  "Answer with exactly one word: true or false.";

const classifyOutcome = (runs: readonly ProbeTurnRun[]): ProbeOutcome => {
  const failure = runs.find((run) => run.record.outcome !== "ok");
  return failure ? failure.record.outcome : "ok";
};

const scenarioResult = (
  scenario: Pick<ProbeScenario, "id" | "group" | "style" | "description" | "expectedOutcome">,
  ctx: ProbeContext,
  startedAt: Date,
  runs: readonly ProbeTurnRun[],
  notes: readonly string[] = [],
  failure: string | null = null,
): ProbeScenarioResult => ({
  id: scenario.id,
  group: scenario.group,
  style: scenario.style,
  description: scenario.description,
  expectedOutcome: scenario.expectedOutcome,
  outcome: failure ? "failed" : classifyOutcome(runs),
  startedAt: startedAt.toISOString(),
  durationMs: ctx.now().getTime() - startedAt.getTime(),
  turns: runs.map((run) => run.record),
  verdict: runs.find((run) => run.verdict !== null)?.verdict ?? null,
  failure,
  notes,
});

export const PROBE_SCENARIOS: readonly ProbeScenario[] = [
  {
    id: "reasoning.effort.low",
    group: "reasoning",
    style: "generic",
    description: "Reasoning effort 'low': does gpt-oss return reasoning plus final content?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user("What is 2 + 2? Answer with only the number.");
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[0], ctx, started, [turn], ["effort=low"]);
    },
  },
  {
    id: "reasoning.effort.medium",
    group: "reasoning",
    style: "generic",
    description: "Reasoning effort 'medium' (the model default): reasoning plus final content?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user("What is 2 + 2? Answer with only the number.");
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        reasoningEffort: "medium",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[1], ctx, started, [turn], ["effort=medium"]);
    },
  },
  {
    id: "reasoning.effort.high",
    group: "reasoning",
    style: "generic",
    description: "Reasoning effort 'high': reasoning plus final content?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user("What is 2 + 2? Answer with only the number.");
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        reasoningEffort: "high",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[2], ctx, started, [turn], ["effort=high"]);
    },
  },
  {
    id: "reasoning.replay.after-final",
    group: "replay",
    style: "generic",
    description: "After a completed final answer a follow-up replays normalized state without old analysis.",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const first = user("What is 2 + 2? Answer with only the number.");
      const t1 = await ctx.runTurn(first, {
        style: "generic",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      if (t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[3], ctx, started, [t1]);
      const withAnswer = advanceConversation(first, t1.normalized);
      const next = appendUser(withAnswer, "What is 9 / 2? Answer with only the number.");
      const replayed = dropAnalysisBeforeCompletedFinal(next);
      const t2 = await ctx.runTurn(replayed, {
        style: "generic",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(
        PROBE_SCENARIOS[3],
        ctx,
        started,
        [t1, t2],
        [`analysisLinesAfterFinal=${analysisLineCount(replayed)}`, "wireAnalysisEcho=false"],
      );
    },
  },
  {
    id: "reasoning.replay.echo",
    group: "replay",
    style: "generic",
    description: "Deliberate echo of reasoning_content in request history: accepted or rejected at the boundary?",
    expectedOutcome: "upstream_rejected",
    run: async (ctx) => {
      const started = ctx.now();
      const first = user("What is 2 + 2? Answer with only the number.");
      const t1 = await ctx.runTurn(first, {
        style: "generic",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      if (t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[4], ctx, started, [t1]);
      const echoBody: Record<string, unknown> = {
        model: HARMONY_CEREBRAS_MODEL,
        messages: [
          { role: "user", content: "What is 2 + 2? Answer with only the number." },
          {
            role: "assistant",
            content: t1.normalized.content ?? "",
            ...(t1.normalized.analysis.length > 0 ? { reasoning_content: t1.normalized.analysis.join("\n") } : {}),
          },
          { role: "user", content: "What is 9 / 2? Answer with only the number." },
        ],
        stream: false,
        max_completion_tokens: MAX_TOKENS,
      };
      const t2 = await ctx.runRaw(echoBody, first);
      const note = t2.record.outcome === "upstream_rejected"
        ? `boundary evidence: reasoning_content rejected with status ${t2.record.status}`
        : `boundary evidence: reasoning_content accepted (status ${t2.record.status})`;
      return scenarioResult(PROBE_SCENARIOS[4], ctx, started, [t1, t2], [note]);
    },
  },
  {
    id: "tools.generic.sequence",
    group: "tools",
    style: "generic",
    description: "Generic tools: one call, normalized tool-call shape, result replay, then a final answer.",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const t1 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call = firstToolCallFrom(t1);
      if (call === null) return scenarioResult(PROBE_SCENARIOS[5], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(
        advanceConversation(conversation, t1.normalized!),
        call.id,
        call.name,
        WEATHER_RESULT,
      );
      const t2 = await ctx.runTurn(withResult, {
        style: "generic",
        tools: [WEATHER_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const notes = [
        `call.id='${call.id}'`,
        `call.name='${call.name}'`,
        `call.argumentsJsonValid=${argsJsonValid(call.arguments)}`,
        `toolResultTurns=${withResult.turns.filter((turn) => turn.role === "tool").length}`,
      ];
      return scenarioResult(PROBE_SCENARIOS[5], ctx, started, [t1, t2], notes);
    },
  },
  {
    id: "tools.native.sequence",
    group: "tools",
    style: "native",
    description:
      "Native Harmony: tools rendered as Harmony types in the developer message, result replayed via tool role.",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const t1 = await ctx.runTurn(conversation, {
        style: "native",
        tools: [WEATHER_TOOL],
        instructions: "You are a weather assistant with access to get_weather.",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call = firstToolCallFrom(t1);
      if (call === null) return scenarioResult(PROBE_SCENARIOS[6], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(
        advanceConversation(conversation, t1.normalized!),
        call.id,
        call.name,
        WEATHER_RESULT,
      );
      const t2 = await ctx.runTurn(withResult, {
        style: "native",
        tools: [WEATHER_TOOL],
        instructions: "You are a weather assistant with access to get_weather.",
        maxCompletionTokens: MAX_TOKENS,
      });
      const parsedFromContent = t1.record.response?.toolCalls.length === 0;
      return scenarioResult(
        PROBE_SCENARIOS[6],
        ctx,
        started,
        [t1, t2],
        [
          `toolCallsFromWire=${t1.record.response?.toolCalls.length ?? 0}`,
          `toolCallsParsedFromContent=${String(parsedFromContent)}`,
          "resultReplayStyle=tool-role",
        ],
      );
    },
  },
  {
    id: "tools.native.user-result",
    group: "tools",
    style: "native",
    description: "Native Harmony with the tool result replayed as a user message (no tool role).",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const t1 = await ctx.runTurn(conversation, {
        style: "native",
        tools: [WEATHER_TOOL],
        instructions: "You are a weather assistant with access to get_weather.",
        nativeToolResultStyle: "user-role",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call = firstToolCallFrom(t1);
      if (call === null) return scenarioResult(PROBE_SCENARIOS[7], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(
        advanceConversation(conversation, t1.normalized!),
        call.id,
        call.name,
        WEATHER_RESULT,
      );
      const t2 = await ctx.runTurn(withResult, {
        style: "native",
        tools: [WEATHER_TOOL],
        instructions: "You are a weather assistant with access to get_weather.",
        nativeToolResultStyle: "user-role",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[7], ctx, started, [t1, t2], ["resultReplayStyle=user-role"]);
    },
  },
  {
    id: "tools.generic.consecutive",
    group: "tools",
    style: "generic",
    description: "Two consecutive tool turns: call, result, call, result, then a final answer.",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      let conversation = user(
        `${WEATHER_QUESTION} After you receive the weather, call save_note with the result.`,
      );
      const t1 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call1 = firstToolCallFrom(t1);
      if (call1 === null) return scenarioResult(PROBE_SCENARIOS[8], ctx, started, [t1], ["no first tool call"]);
      conversation = appendToolResult(
        advanceConversation(conversation, t1.normalized!),
        call1.id,
        call1.name,
        WEATHER_RESULT,
      );
      const t2 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call2 = firstToolCallFrom(t2);
      if (call2 === null) return scenarioResult(PROBE_SCENARIOS[8], ctx, started, [t1, t2], ["no second tool call"]);
      conversation = appendToolResult(
        advanceConversation(conversation, t2.normalized!),
        call2.id,
        call2.name,
        '{"text": "San Francisco: sunny, 20C"}',
      );
      const t3 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const notes = [
        `analysisLinesState=${analysisLineCount(conversation)}`,
        `pendingToolCallsAtEnd=${pendingToolCallCount(conversation)}`,
        `secondCall.name='${call2.name}'`,
      ];
      return scenarioResult(PROBE_SCENARIOS[8], ctx, started, [t1, t2, t3], notes);
    },
  },
  {
    id: "strictness.mixed",
    group: "strictness",
    style: "generic",
    description: "Two tools with different strict values in one request: accepted or rejected?",
    expectedOutcome: "upstream_rejected",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [{ ...WEATHER_TOOL, strict: true }, { ...NOTE_TOOL, strict: false }],
        toolStrictnessMode: "preserve",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const values = turn.record.request?.toolStrictnessValues ?? [];
      return scenarioResult(PROBE_SCENARIOS[9], ctx, started, [turn], [`strictnessValues=${values.join(",")}`]);
    },
  },
  {
    id: "strictness.all-false",
    group: "strictness",
    style: "generic",
    description: "All tools normalized to strict=false: accepted?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[10], ctx, started, [turn], ["strictness=all-false"]);
    },
  },
  {
    id: "strictness.all-true",
    group: "strictness",
    style: "generic",
    description: "All tools normalized to strict=true: accepted?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(WEATHER_QUESTION);
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        toolStrictnessMode: "normalize-true",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[11], ctx, started, [turn], ["strictness=all-true"]);
    },
  },
  {
    id: "structured.json-object",
    group: "structured",
    style: "generic",
    description: "Structured output alone with response_format json_object: JSON in final content?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user('Return a JSON object with an integer field "answer" equal to 4.');
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        responseFormat: { type: "json_object" },
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const preview = turn.record.response?.contentPreview ?? null;
      let jsonOk = false;
      try {
        JSON.parse(preview ?? "");
        jsonOk = true;
      } catch {
        jsonOk = false;
      }
      return scenarioResult(PROBE_SCENARIOS[12], ctx, started, [turn], [`contentJsonValid=${jsonOk}`]);
    },
  },
  {
    id: "structured.json-schema",
    group: "structured",
    style: "generic",
    description: "Structured output alone with response_format json_schema: JSON matching the schema?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user('Return a JSON object with an integer field "answer" equal to 4.');
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        responseFormat: {
          type: "json_schema",
          json_schema: { name: "answer_response", strict: true, schema: ANSWER_SCHEMA },
        },
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[13], ctx, started, [turn], ["schema=answer_response"]);
    },
  },
  {
    id: "structured.native-formats",
    group: "structured",
    style: "native",
    description: "Native Harmony: schema rendered as a Response Format in the developer message.",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user('Return a JSON object with an integer field "answer" equal to 4.');
      const turn = await ctx.runTurn(conversation, {
        style: "native",
        nativeResponseFormat: {
          formatName: "answer_response",
          description: "The computed answer",
          schema: ANSWER_SCHEMA,
        },
        instructions: "You are a math assistant. Use the Response Format below.",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[14], ctx, started, [turn], ["responseFormat=developer"]);
    },
  },
  {
    id: "structured.with-tools",
    group: "structured",
    style: "generic",
    description: "Tools combined with response_format json_schema: supported or rejected?",
    expectedOutcome: "upstream_rejected",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user("Call get_weather for San Francisco, then return a JSON object with the temperature.");
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL],
        responseFormat: {
          type: "json_schema",
          json_schema: { name: "weather_answer", strict: true, schema: ANSWER_SCHEMA },
        },
        combinationPolicy: "probe",
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      return scenarioResult(PROBE_SCENARIOS[15], ctx, started, [turn], ["combinationPolicy=probe"]);
    },
  },
  {
    id: "parallel.native",
    group: "parallel",
    style: "native",
    description: "Native Harmony: prompt asks for two parallel calls in one turn; how many calls are returned?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(
        "Call get_weather twice in the same response: location San Francisco and location Tokyo.",
      );
      const turn = await ctx.runTurn(conversation, {
        style: "native",
        tools: [WEATHER_TOOL],
        instructions: "You are a weather assistant with access to get_weather.",
        maxCompletionTokens: MAX_TOKENS,
      });
      const count = turn.record.response?.toolCalls.length ?? 0;
      return scenarioResult(PROBE_SCENARIOS[16], ctx, started, [turn], [`toolCallCount=${count}`]);
    },
  },
  {
    id: "parallel.generic-flag",
    group: "parallel",
    style: "generic",
    description: "Generic with parallel_tool_calls=true: accepted? how many calls are returned?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const conversation = user(
        "Call get_weather twice in the same response: location San Francisco and location Tokyo.",
      );
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL],
        parallelToolCalls: true,
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const count = turn.record.response?.toolCalls.length ?? 0;
      return scenarioResult(PROBE_SCENARIOS[17], ctx, started, [turn], [
        `toolCallCount=${count}`,
        "parallelToolCalls=true",
      ]);
    },
  },
  {
    id: "classifier.low",
    group: "classifier",
    style: "classifier",
    description: "Zero-tool bounded classifier request at reasoning effort low: literal true/false verdict?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const turn = await ctx.runClassifier({
        observation: CLASSIFIER_OBSERVATION,
        decisionDefinition: CLASSIFIER_DECISION,
        reasoningEffort: "low",
      });
      return scenarioResult(PROBE_SCENARIOS[18], ctx, started, [turn], ["maxOutputTokens=128"]);
    },
  },
  {
    id: "classifier.medium",
    group: "classifier",
    style: "classifier",
    description: "Same zero-tool bounded classifier request at reasoning effort medium: literal true/false verdict?",
    expectedOutcome: "ok",
    run: async (ctx) => {
      const started = ctx.now();
      const turn = await ctx.runClassifier({
        observation: CLASSIFIER_OBSERVATION,
        decisionDefinition: CLASSIFIER_DECISION,
        reasoningEffort: "medium",
      });
      return scenarioResult(PROBE_SCENARIOS[19], ctx, started, [turn], ["maxOutputTokens=128"]);
    },
  },
];

/** Finds the newest tool call inside a freshly executed turn's normalized response. */
const firstToolCallFrom = (run: ProbeTurnRun) => {
  if (run.normalized === null) return null;
  return run.normalized.toolCalls.length > 0 ? run.normalized.toolCalls[0] : null;
};
