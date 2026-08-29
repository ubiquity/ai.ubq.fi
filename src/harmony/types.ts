/**
 * Shared types for the isolated Harmony/Cerebras protocol adapter (plan m01).
 *
 * The GPT-OSS models were trained on the Harmony response format
 * (https://github.com/openai/harmony).  This module defines the normalized
 * conversation and turn model used by the adapter, the probe manifest, and
 * their deterministic fake-transport tests.  It must not be imported by the
 * gateway routes: this is probe and harness infrastructure owned by the
 * Harmony agent-foundation work.
 */

import { CEREBRAS_GPT_OSS_120B_MODEL } from "../cerebras.ts";

/** Exact upstream model used by every Harmony protocol probe. */
export const HARMONY_CEREBRAS_MODEL = CEREBRAS_GPT_OSS_120B_MODEL;

/** Reasoning efforts the plan is allowed to experiment with. */
export type HarmonyReasoningEffort = "low" | "medium" | "high";

/** Default effort used when a request does not specify one. */
export const HARMONY_DEFAULT_REASONING_EFFORT: HarmonyReasoningEffort = "medium";

/** Harmony assistant channels. */
export type HarmonyChannel = "analysis" | "commentary" | "final";

/**
 * Call styles the adapter can produce.
 *
 * - `generic` forwards the official Chat Completions `tools` (and top-level
 *   `reasoning_effort` / `response_format`) and lets the provider translate
 *   between OpenAI tool calls and Harmony semantics.  This is the gateway's
 *   current GPT-OSS behavior.
 * - `native` renders tools and the reasoning level *in* the system/developer
 *   messages exactly like the Harmony renderer, sends no `tools` field, and
 *   relies on Harmony-native output (auto-parsed by the provider or parsed by
 *   this adapter).
 */
export type HarmonyCallStyle = "generic" | "native";

/**
 * How exposed tools are normalized for the provider.
 *
 * Cerebras requires every tool in one request to carry the same `strict`
 * value ("Tools with mixed values for 'strict' are not allowed"), so the
 * adapter normalizes by default.  `preserve` exists only for protocol probes
 * that must record the upstream rejection.
 */
export type ToolStrictnessMode = "normalize-false" | "normalize-true" | "preserve";

/** A model-facing tool definition (official OpenAI Chat Completions shape). */
export type ToolDefinition = Readonly<{
  name: string;
  description: string;
  parameters: Record<string, unknown>;
  strict?: boolean;
}>;

/** Normalized tool call inside an assistant turn. */
export type ToolCall = Readonly<{
  id: string;
  /** Tool name without the `functions.` Harmony namespace prefix. */
  name: string;
  /** Opaque JSON string, matched by the shared application pipeline. */
  arguments: string;
}>;

/**
 * One normalized assistant turn.  `analysis` is the private chain of thought
 * (Harmony `analysis` channel) and never leaves the adapter; `content` is the
 * user-visible final/commentary text.
 */
export type AssistantTurn = Readonly<{
  role: "assistant";
  content: string | null;
  analysis: readonly string[];
  toolCalls: readonly ToolCall[];
  finishReason: string | null;
}>;

/** Normalized tool result turn (OpenAI `tool` role). */
export type ToolResultTurn = Readonly<{
  role: "tool";
  toolCallId: string;
  name: string;
  content: string;
}>;

/** A conversation turn the adapter is allowed to hold in state. */
export type ConversationTurn = Readonly<
  | { role: "system"; content: string }
  | { role: "developer"; content: string }
  | { role: "user"; content: string }
  | AssistantTurn
  | ToolResultTurn
>;

/**
 * Parsed Harmony assistant output segments, in emission order.
 *
 * - `reasoning`  : `analysis` channel text (private chain of thought).
 * - `commentary` : `commentary` channel text without a recipient (preambles).
 * - `tool_call`  : `commentary`/`analysis` channel with a function recipient.
 * - `final`      : `final` channel text (the answer to show the user).
 */
export type HarmonyTurn = Readonly<
  | { kind: "reasoning"; text: string }
  | { kind: "commentary"; text: string }
  | { kind: "tool_call"; recipient: string; name: string; arguments: string }
  | { kind: "final"; text: string }
>;

/** Shape of one raw assistant message parsed from Harmony text. */
export type HarmonyRawMessage = Readonly<{
  role: string;
  channel: HarmonyChannel | null;
  recipient: string | null;
  constrain: string | null;
  content: string;
  stoppedBy: "<|end|>" | "<|call|>" | "<|return|>" | "truncated";
}>;

/** Result of parsing raw assistant output text as Harmony. */
export type HarmonyParseResult = Readonly<{
  messages: readonly HarmonyRawMessage[];
  turns: readonly HarmonyTurn[];
  truncated: boolean;
}>;

/** What a normalized assistant response looked like on the wire. */
export type AssistantResponseShape = Readonly<{
  contentPresent: boolean;
  contentChars: number;
  reasoningField: "reasoning_content" | "reasoning" | "none";
  reasoningChars: number;
  toolCallsField: boolean;
  toolCallCount: number;
  finishReason: string | null;
  refusal: boolean;
}>;

/** Normalized assistant completion from a provider Chat Completions payload. */
export type NormalizedAssistantResponse = Readonly<{
  turns: readonly HarmonyTurn[];
  analysis: readonly string[];
  content: string | null;
  toolCalls: readonly ToolCall[];
  finishReason: string | null;
  refusal: string | null;
  shape: AssistantResponseShape;
  id: string;
  model: string;
  created: number;
}>;

/** Official OpenAI Chat Completions response format parameter. */
export type ResponseFormatParam = Readonly<
  | { type: "json_object" }
  | {
    type: "json_schema";
    json_schema: { name: string; strict?: boolean; schema: Record<string, unknown>; description?: string };
  }
>;

/** Structured-output description for the native (Harmony-rendered) style. */
export type NativeResponseFormat = Readonly<{
  formatName: string;
  description?: string;
  schema: Record<string, unknown>;
}>;

/** Policy for combining tools with a structured `response_format`. */
export type CombinationPolicy = "error" | "probe";

/**
 * Adapter-domain error.  `code` lets tests and the probe runner distinguish
 * local policy rejections from upstream failures.
 */
export type HarmonyAdapterErrorCode =
  | "unproven-combination"
  | "mixed-strictness-requested"
  | "invalid-request"
  | "invalid-upstream-response"
  | "no-model-output";

export class HarmonyAdapterError extends Error {
  readonly code: HarmonyAdapterErrorCode;

  constructor(message: string, code: HarmonyAdapterErrorCode) {
    super(message);
    this.name = "HarmonyAdapterError";
    this.code = code;
  }
}
