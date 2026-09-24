// Harmony probe scenarios, split out of src/harmony/probes.ts.

import {
  advanceConversation,
  analysisLineCount,
  appendToolResult,
  appendUser,
  dropAnalysisBeforeCompletedFinal,
  pendingToolCallCount,
} from "./conversation.ts";
import type { HarmonyCallStyle } from "./types.ts";
import { HARMONY_CEREBRAS_MODEL } from "./types.ts";

// ---------------------------------------------------------------------------
// Sanitized record shapes
// ---------------------------------------------------------------------------
import {
  ANSWER_SCHEMA,
  argsJsonValid,
  NOTE_TOOL,
  type ProbeContext,
  type ProbeGroup,
  type ProbeOutcome,
  type ProbeScenarioResult,
  type ProbeTurnRun,
  user,
  WEATHER_TOOL,
} from "./probes.ts";

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

const CLASSIFIER_DECISION =
  "Decide whether the run made material progress since the previous evaluation. " +
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
  failure: string | null = null
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
      return scenarioResult(PROBE_SCENARIOS[3], ctx, started, [t1, t2], [`analysisLinesAfterFinal=${analysisLineCount(replayed)}`, "wireAnalysisEcho=false"]);
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
      const note =
        t2.record.outcome === "upstream_rejected"
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
      if (call === null || t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[5], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(advanceConversation(conversation, t1.normalized), call.id, call.name, WEATHER_RESULT);
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
    description: "Native Harmony: tools rendered as Harmony types in the developer message, result replayed via tool role.",
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
      if (call === null || t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[6], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(advanceConversation(conversation, t1.normalized), call.id, call.name, WEATHER_RESULT);
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
        ]
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
      if (call === null || t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[7], ctx, started, [t1], ["no tool call returned"]);
      const withResult = appendToolResult(advanceConversation(conversation, t1.normalized), call.id, call.name, WEATHER_RESULT);
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
      let conversation = user(`${WEATHER_QUESTION} After you receive the weather, call save_note with the result.`);
      const t1 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call1 = firstToolCallFrom(t1);
      if (call1 === null || t1.normalized === null) return scenarioResult(PROBE_SCENARIOS[8], ctx, started, [t1], ["no first tool call"]);
      conversation = appendToolResult(advanceConversation(conversation, t1.normalized), call1.id, call1.name, WEATHER_RESULT);
      const t2 = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL, NOTE_TOOL],
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const call2 = firstToolCallFrom(t2);
      if (call2 === null || t2.normalized === null) return scenarioResult(PROBE_SCENARIOS[8], ctx, started, [t1, t2], ["no second tool call"]);
      conversation = appendToolResult(advanceConversation(conversation, t2.normalized), call2.id, call2.name, '{"text": "San Francisco: sunny, 20C"}');
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
        tools: [
          { ...WEATHER_TOOL, strict: true },
          { ...NOTE_TOOL, strict: false },
        ],
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
        // Unparseable content leaves jsonOk at its initial false.
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
      const conversation = user("Call get_weather twice in the same response: location San Francisco and location Tokyo.");
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
      const conversation = user("Call get_weather twice in the same response: location San Francisco and location Tokyo.");
      const turn = await ctx.runTurn(conversation, {
        style: "generic",
        tools: [WEATHER_TOOL],
        parallelToolCalls: true,
        reasoningEffort: "low",
        maxCompletionTokens: MAX_TOKENS,
      });
      const count = turn.record.response?.toolCalls.length ?? 0;
      return scenarioResult(PROBE_SCENARIOS[17], ctx, started, [turn], [`toolCallCount=${count}`, "parallelToolCalls=true"]);
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

/**
 * Finds the newest tool call inside a freshly executed turn's normalized
 * response.  A non-null call therefore always comes with a non-null
 * `normalized` response on the same run.
 */
const firstToolCallFrom = (run: ProbeTurnRun) => {
  if (run.normalized === null) return null;
  return run.normalized.toolCalls.length > 0 ? run.normalized.toolCalls[0] : null;
};
