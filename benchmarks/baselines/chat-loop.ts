/**
 * Shared deterministic Chat-Completions agent loop for the m03 baselines.
 *
 * Adapters A (current gateway GPT-OSS chat) and D (generic strong control)
 * both drive the same loop: build a bounded conversation, dispatch one
 * Chat Completions request through an injected transport, normalize the
 * choice, execute any tool calls against the benchmark tool layer, and
 * record every event through the shared trajectory sink. Everything a run
 * records goes through `ctx.record`, so the runner derives metrics exactly
 * as for the `reference` adapter; success is still decided only by the
 * declared verification and oracle.
 *
 * The loop is deliberately small and adversarial-input tolerant: malformed
 * tool arguments become `valid:false` calls (never a crash), the whole-run
 * signal aborts in-flight tool work, and a defensive request cap prevents a
 * misbehaving model from spinning without tool calls.
 */

import { type AdapterRunContext, TaskTimeoutError, type ToolResult } from "../adapter.ts";
import type { ModelRequestEvent } from "../schemas.ts";
import { BaselineAdapterError, BaselineUpstreamError, sanitizedUpstreamError } from "./errors.ts";
import type { ChatTransport } from "./transport.ts";
import { type CanonicalToolDefinition, executeBaselineTool, validateCanonicalToolArgs } from "./tools.ts";

export interface ToolCallWire {
  id: string;
  name: string;
  /** Opaque JSON arguments exactly as received on the wire. */
  arguments: string;
}

export interface ParsedChatCompletion {
  content: string | null;
  toolCalls: ToolCallWire[];
  finishReason: string | null;
  usage: { inputTokens: number; outputTokens: number } | null;
}

/** A request message pushed to the live model (OpenAI Chat shape). */
export type ChatMessage = Record<string, unknown>;

/** Adapter-specific pieces of one chat agent configuration. */
export interface ChatAgentSpec {
  model: string;
  tools: readonly CanonicalToolDefinition[];
  transport: ChatTransport;
  systemMessage(ctx: AdapterRunContext): string;
  userMessage(ctx: AdapterRunContext): string;
  /** Builds the complete Chat Completions body for one turn. */
  buildRequest(ctx: AdapterRunContext, messages: readonly ChatMessage[]): Record<string, unknown>;
  /** Normalizes a 2xx payload into a choice or a deterministic error. */
  parseCompletion(value: unknown): ParsedChatCompletion | { error: string };
  /** Defensive cap so a run cannot spin forever (default 200). */
  maxRequests?: number;
}

const DEFAULT_MAX_REQUESTS = 200;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

/** Parses wire arguments into a record; invalid JSON never throws. */
export function parseToolArguments(text: string): Record<string, unknown> {
  try {
    const value: unknown = JSON.parse(text);
    if (isRecord(value)) return value;
    return { value };
  } catch {
    return { __invalid_json: text };
  }
}

/**
 * Runs one bounded agent session. Emits, in order:
 * `model_request` → `model_response` → (`tool_call` → `tool_result`)* until
 * the model returns a turn without tool calls.
 */
export async function runChatAgentLoop(ctx: AdapterRunContext, spec: ChatAgentSpec): Promise<void> {
  const messages: ChatMessage[] = [
    { role: "system", content: spec.systemMessage(ctx) },
    { role: "user", content: spec.userMessage(ctx) },
  ];
  const maxRequests = spec.maxRequests ?? DEFAULT_MAX_REQUESTS;
  let seq = 0;

  for (;;) {
    if (ctx.signal.aborted) throw new TaskTimeoutError(ctx.task.timeout_ms);
    if (seq >= maxRequests) {
      throw new BaselineAdapterError(
        `model request limit exceeded (${maxRequests}); the model never returned a final turn`,
        "request-limit",
      );
    }
    seq += 1;

    if (ctx.signal.aborted) throw new TaskTimeoutError(ctx.task.timeout_ms);
    const request: ModelRequestEvent = {
      type: "model_request",
      at: ctx.time(),
      id: seq,
      model: spec.model,
      message_count: messages.length,
      input_tokens: 0,
      output_tokens: 0,
      tool_count: spec.tools.length,
    };
    ctx.record(request);
    const response = await spec.transport(spec.buildRequest(ctx, messages), { signal: ctx.signal });
    const body = await response.json().catch(() => null);
    if (!response.ok) {
      const info = sanitizedUpstreamError(body);
      throw new BaselineUpstreamError(response.status, info.code, info.message);
    }
    const parsed = spec.parseCompletion(body);
    if ("error" in parsed) {
      throw new BaselineAdapterError(
        `could not normalize the model response: ${parsed.error}`,
        "invalid-upstream-response",
      );
    }

    const usage = parsed.usage ?? { inputTokens: 0, outputTokens: 0 };
    request.input_tokens = usage.inputTokens;
    request.output_tokens = usage.outputTokens;
    ctx.record({
      type: "model_response",
      at: ctx.time(),
      request_id: seq,
      content: parsed.content ?? undefined,
      tool_calls: parsed.toolCalls.map((call) => ({
        id: call.id,
        name: call.name,
        arguments: parseToolArguments(call.arguments),
      })),
      finish_reason: parsed.finishReason ?? undefined,
    });

    if (parsed.toolCalls.length === 0) return;

    messages.push({
      role: "assistant",
      content: parsed.content ?? null,
      tool_calls: parsed.toolCalls.map((call) => ({
        id: call.id,
        type: "function",
        function: { name: call.name, arguments: call.arguments },
      })),
    });

    for (const call of parsed.toolCalls) {
      ctx.checkToolLimit();
      if (ctx.signal.aborted) throw new TaskTimeoutError(ctx.task.timeout_ms);
      const args = parseToolArguments(call.arguments);
      const validated = validateCanonicalToolArgs(call.name, args);
      ctx.record({
        type: "tool_call",
        at: ctx.time(),
        id: call.id,
        tool: call.name,
        arguments: args,
        valid: validated.valid,
        invalid_reason: validated.valid ? undefined : validated.reason,
      });
      const started = Date.now();
      let result: ToolResult;
      if (!validated.valid) {
        result = { ok: false, error: `invalid arguments: ${validated.reason}`, error_code: "invalid_args" };
      } else {
        try {
          result = await executeBaselineTool(ctx.workspace, call.name, args, ctx.signal);
        } catch (err) {
          result = { ok: false, error: (err as Error).message, error_code: "exec_failed" };
        }
      }
      ctx.record({
        type: "tool_result",
        at: ctx.time(),
        id: call.id,
        ok: result.ok,
        output: result.output,
        error: result.error,
        error_code: result.error_code,
        duration_ms: Date.now() - started,
      });
      messages.push({
        role: "tool",
        tool_call_id: call.id,
        content: result.ok ? (result.output ?? "") : `error: ${result.error ?? "unknown error"}`,
      });
    }
  }
}
