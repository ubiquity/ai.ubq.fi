/**
 * Baseline A: the current gateway GPT-OSS Chat Completions behavior.
 *
 * This adapter mirrors what the gateway does today for `gpt-oss-120b`:
 *
 * - the request body is the gateway's `handleCerebrasChatCompletions` shape
 *   (`src/openai.ts`): official Chat Completions `messages`/`tools` fields
 *   preserved, `model` forced to `gpt-oss-120b`, `stream: false`, and
 *   `reasoning_effort` defaulting to `medium` when the client omits it;
 * - the transport is the gateway's own `fetchCerebrasChatCompletions`
 *   (`src/cerebras.ts`): the existing `CEREBRAS_API_KEY`, the gateway
 *   deadline, Cerebras schema projection and the exact error normalization;
 * - the response is normalized with the gateway's own
 *   `normalizeCerebrasChatCompletion`, so only the wheels the gateway
 *   accepts reach the benchmark tool layer.
 *
 * Like the gateway, `reasoning_effort: "none"` is rejected deterministically.
 * The transport is injectable; focused tests substitute a fake transport and
 * the runner refuses this adapter by default because
 * `requiresExternalInference` is true.
 */

import { type AdapterRunContext, type BenchmarkAdapter } from "../adapter.ts";
import { CEREBRAS_GPT_OSS_120B_MODEL, normalizeCerebrasChatCompletion } from "../../src/cerebras.ts";
import { type ChatMessage, type ParsedChatCompletion, runChatAgentLoop, type ToolCallWire } from "./chat-loop.ts";
import { BaselineAdapterError } from "./errors.ts";
import { type ChatTransport, gatewayChatTransport } from "./transport.ts";
import { canonicalToolDefinitions } from "./tools.ts";

export const GATEWAY_GPT_OSS_MODEL = CEREBRAS_GPT_OSS_120B_MODEL;

/** Efforts accepted by the gateway for GPT-OSS (m01 probe scope). */
export const GATEWAY_GPT_OSS_EFFORTS = ["low", "medium", "high"] as const;
export type GatewayGptOssEffort = (typeof GATEWAY_GPT_OSS_EFFORTS)[number];

export interface BaselineAOptions {
  /** Injected transport; defaults to the live gateway transport. */
  transport?: ChatTransport;
  /** Reasoning effort sent on every request; default `medium` like the gateway. */
  reasoningEffort?: GatewayGptOssEffort;
  /** Cerebras requires one strictness value per request; default false. */
  toolStrictness?: boolean;
  maxCompletionTokens?: number;
  maxRequests?: number;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const SYSTEM_MESSAGE =
  "You are a deterministic benchmark agent. Complete the user's task inside the disposable workspace " +
  "using the provided tools. The benchmark runner (never the model) verifies success, so do not claim " +
  "completion in prose; keep working until the declared task is fully done, then produce a short final answer.";

function userMessage(ctx: AdapterRunContext): string {
  return `${ctx.task.description}\n\nThe workspace is the current working directory for shell tools.`;
}

function normalizeGatewayCompletion(value: unknown): ParsedChatCompletion | { error: string } {
  const normalized = normalizeCerebrasChatCompletion(value, CEREBRAS_GPT_OSS_120B_MODEL);
  if (!normalized.ok) return { error: normalized.message };
  const payload = normalized.value as Record<string, unknown>;
  const choices = payload.choices as Record<string, unknown>[];
  const choice = choices[0] as Record<string, unknown>;
  const message = choice.message as Record<string, unknown>;
  const content = typeof message.content === "string" ? message.content : null;
  const toolCalls: ToolCallWire[] = [];
  if (Array.isArray(message.tool_calls)) {
    for (const raw of message.tool_calls as Record<string, unknown>[]) {
      const fn = raw.function as Record<string, unknown>;
      toolCalls.push({
        id: typeof raw.id === "string" ? raw.id : `gateway-call-${toolCalls.length + 1}`,
        name: typeof fn?.name === "string" ? fn.name : "(unknown)",
        arguments: typeof fn?.arguments === "string" ? fn.arguments : "{}",
      });
    }
  }
  const usageRaw = payload.usage;
  const usage = isRecord(usageRaw)
    ? {
      inputTokens: typeof usageRaw.prompt_tokens === "number" ? usageRaw.prompt_tokens : 0,
      outputTokens: typeof usageRaw.completion_tokens === "number" ? usageRaw.completion_tokens : 0,
    }
    : null;
  return {
    content,
    toolCalls,
    finishReason: typeof choice.finish_reason === "string" ? choice.finish_reason : null,
    usage,
  };
}

export function createBaselineA(options: BaselineAOptions = {}): BenchmarkAdapter {
  // Runtime value may be anything; the gateway's own rule rejects "none"
  // before any request is dispatched.
  const effortValue = (options.reasoningEffort ?? "medium") as string;
  if (effortValue === "none") {
    throw new BaselineAdapterError(
      "reasoning_effort 'none' is not supported for gpt-oss-120b. Use low, medium, or high.",
      "invalid-config",
    );
  }
  if (!(GATEWAY_GPT_OSS_EFFORTS as readonly string[]).includes(effortValue)) {
    throw new BaselineAdapterError(
      `reasoning_effort must be one of ${GATEWAY_GPT_OSS_EFFORTS.join(", ")} for gpt-oss-120b`,
      "invalid-config",
    );
  }
  const effort = effortValue as GatewayGptOssEffort;
  const transport = options.transport ?? gatewayChatTransport();
  const strict = options.toolStrictness ?? false;
  const tools = canonicalToolDefinitions(strict);

  return {
    configId: "A",
    name: "gateway-gpt-oss-chat",
    description: "Current gateway GPT-OSS Chat Completions behavior (gpt-oss-120b via the gateway transport, " +
      "official tools contract, reasoning_effort medium). Live calls require the approved inference gate.",
    requiresExternalInference: true,
    async run(ctx: AdapterRunContext): Promise<void> {
      await runChatAgentLoop(ctx, {
        model: CEREBRAS_GPT_OSS_120B_MODEL,
        tools,
        transport,
        systemMessage: () => SYSTEM_MESSAGE,
        userMessage,
        maxRequests: options.maxRequests,
        buildRequest: (_ctx, messages: readonly ChatMessage[]): Record<string, unknown> => {
          const body: Record<string, unknown> = {
            model: CEREBRAS_GPT_OSS_120B_MODEL,
            messages: [...messages],
            stream: false,
            reasoning_effort: effort,
            tools: tools.map((tool) => ({
              type: "function",
              function: {
                name: tool.name,
                description: tool.description,
                parameters: tool.parameters,
                strict: tool.strict,
              },
            })),
          };
          if (options.maxCompletionTokens !== undefined) body.max_completion_tokens = options.maxCompletionTokens;
          return body;
        },
        parseCompletion: normalizeGatewayCompletion,
      });
    },
  };
}

/** Default instance: live gateway transport, refused by the runner. */
export const adapterA: BenchmarkAdapter = createBaselineA();
