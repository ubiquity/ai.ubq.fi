/**
 * Baseline D: generic strong control.
 *
 * D runs the same task contract as A against a *stronger* model through any
 * OpenAI-compatible Chat Completions endpoint. Per the plan, the control
 * stays generic until the repository has an owner-approved available model:
 *
 * - the adapter adds **no** secret, environment variable, CLI flag or paid
 *   provider on its own; the key is supplied through the configuration
 *   object by whoever owns the approved live gate;
 * - the default instance carries a placeholder model, a null transport and
 *   no key, so `run()` refuses deterministically;
 * - focused tests inject a fake transport plus a concrete model string, so
 *   the full loop is exercised offline against the real tool layer.
 */

import { type AdapterRunContext, type BenchmarkAdapter } from "../adapter.ts";
import { type ChatMessage, type ParsedChatCompletion, runChatAgentLoop, type ToolCallWire } from "./chat-loop.ts";
import { BaselineAdapterError, BaselineNotProvisionedError } from "./errors.ts";
import { type ChatTransport, openAICompatibleTransport } from "./transport.ts";
import { canonicalToolDefinitions } from "./tools.ts";

/** Placeholder until an approved strong control model exists. */
export const CONTROL_MODEL_PLACEHOLDER = "<unapproved-control-model>";
export const CONTROL_BASE_URL_PLACEHOLDER = "https://api.openai.com/v1";

export interface StrongControlOptions {
  /** Model identifier; must be an owner-approved control model for live use. */
  model: string;
  /** Base URL of an OpenAI-compatible chat completions endpoint. */
  baseUrl: string;
  /**
   * API key for the live transport. Passed via configuration only; this
   * module never reads process environment variables.
   */
  apiKey: string | null;
  /**
   * Injected transport. `null` (the default) refuses to run: the live
   * transport is only attached with explicit owner approval.
   */
  transport: ChatTransport | null;
  reasoningEffort?: string;
  maxCompletionTokens?: number;
  maxRequests?: number;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const SYSTEM_MESSAGE =
  "You are a strong control agent for a deterministic benchmark. Complete the user's task inside the " +
  "disposable workspace using the provided tools. The benchmark runner (never the model) verifies " +
  "success, so do not claim completion in prose; keep working until the declared task is fully done, " +
  "then produce a short final answer.";

export function normalizeOpenAICompatibleCompletion(
  value: unknown,
  model: string,
): ParsedChatCompletion | { error: string } {
  if (!isRecord(value)) return { error: "reply is not a Chat Completions object" };
  if (typeof value.model === "string" && value.model !== model) {
    return { error: `upstream returned model ${JSON.stringify(value.model)} instead of ${JSON.stringify(model)}` };
  }
  if (!Array.isArray(value.choices) || value.choices.length === 0) {
    return { error: "reply has no choices" };
  }
  const choice = value.choices[0];
  if (!isRecord(choice)) return { error: "choice is not an object" };
  const message = choice.message;
  if (!isRecord(message)) return { error: "assistant message is missing" };
  const content = typeof message.content === "string" ? message.content : null;
  const toolCalls: ToolCallWire[] = [];
  if (message.tool_calls !== undefined && message.tool_calls !== null) {
    if (!Array.isArray(message.tool_calls)) return { error: "tool_calls is not an array" };
    for (const raw of message.tool_calls) {
      if (!isRecord(raw)) return { error: "tool call is not an object" };
      const fn = raw.function;
      if (!isRecord(fn)) return { error: "tool call function is missing" };
      if (typeof fn.name !== "string" || fn.name.length === 0) return { error: "tool call name is invalid" };
      if (typeof fn.arguments !== "string") return { error: "tool call arguments are not a string" };
      toolCalls.push({
        id: typeof raw.id === "string" && raw.id.length > 0 ? raw.id : `control-call-${toolCalls.length + 1}`,
        name: fn.name,
        arguments: fn.arguments,
      });
    }
  }
  const usage = isRecord(value.usage)
    ? {
      inputTokens: typeof value.usage.prompt_tokens === "number" ? value.usage.prompt_tokens : 0,
      outputTokens: typeof value.usage.completion_tokens === "number" ? value.usage.completion_tokens : 0,
    }
    : null;
  return {
    content,
    toolCalls,
    finishReason: typeof choice.finish_reason === "string" ? choice.finish_reason : null,
    usage,
  };
}

export function createBaselineD(options: StrongControlOptions): BenchmarkAdapter {
  const transport = options.transport ?? openAICompatibleTransport(options.baseUrl, options.apiKey);
  const tools = canonicalToolDefinitions(false);

  return {
    configId: "D",
    name: "strong-control",
    description: "Generic strong control over an owner-approved OpenAI-compatible model; default instance refuses " +
      "to run until the control model and transport are approved (no new secrets or env interfaces).",
    requiresExternalInference: true,
    async run(ctx: AdapterRunContext): Promise<void> {
      if (options.transport === null) {
        throw new BaselineNotProvisionedError(
          "strong control is not provisioned: no transport was configured and the control model is not " +
            `approved. Configure createBaselineD with an approved model (current placeholder: ` +
            `${JSON.stringify(options.model)}) and an explicit transport.`,
        );
      }
      await runChatAgentLoop(ctx, {
        model: options.model,
        tools,
        transport,
        systemMessage: () => SYSTEM_MESSAGE,
        userMessage: (run) =>
          `${run.task.description}\n\nThe workspace is the current working directory for shell tools.`,
        maxRequests: options.maxRequests,
        buildRequest: (_ctx, messages: readonly ChatMessage[]): Record<string, unknown> => {
          const body: Record<string, unknown> = {
            model: options.model,
            messages: [...messages],
            stream: false,
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
          if (options.reasoningEffort !== undefined) body.reasoning_effort = options.reasoningEffort;
          if (options.maxCompletionTokens !== undefined) body.max_completion_tokens = options.maxCompletionTokens;
          return body;
        },
        parseCompletion: (value) => normalizeOpenAICompatibleCompletion(value, options.model),
      });
    },
  };
}

/** Default instance: placeholder model, null transport, always refused. */
export const adapterD: BenchmarkAdapter = createBaselineD({
  model: CONTROL_MODEL_PLACEHOLDER,
  baseUrl: CONTROL_BASE_URL_PLACEHOLDER,
  apiKey: null,
  transport: null,
});

/** Policy note: the default D must stay unapproved until the owner acts. */
export function assertControlModelApproved(model: string): void {
  if (model === CONTROL_MODEL_PLACEHOLDER || model.trim() === "" || model.startsWith("<")) {
    throw new BaselineAdapterError(
      `control model ${JSON.stringify(model)} is not an approved available model`,
      "invalid-config",
    );
  }
}
