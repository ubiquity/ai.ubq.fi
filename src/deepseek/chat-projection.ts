// Responses-to-Chat-Completions projection, split out of src/deepseek_responses.ts.

import { type ChatOnlyResponsesProfile, DEEPSEEK_RESPONSES_PROFILE, type DeepSeekResponsesResult, failure, originalToolName } from "./responses.ts";
import { getString, isRecord } from "../utils.ts";

type ChatContentPart = Record<string, unknown>;

/** One Responses content part mapped onto the Chat content union for one message role. */
const chatContentPart = (part: unknown, role: string): DeepSeekResponsesResult<Readonly<{ text?: string; image?: ChatContentPart }>> => {
  if (!isRecord(part) || Array.isArray(part)) return failure("input.content", "input.content items must be objects");
  const type = getString(part.type);
  if (type === "input_text" || type === "output_text" || type === "text") {
    if (typeof part.text !== "string") return failure("input.content.text", "input.content text must be a string");
    return { ok: true, value: { text: part.text } };
  }
  // The refusal part this adapter emits (`responseMessageContent`) is assistant
  // output replayed as history; it carries its payload in `refusal`. Chat
  // Completions has no refusal content part, so the text replays as the
  // assistant message it is, matching the repository's own Chat-side replay rule
  // for a refusal part. Without this translation a refusal the gateway just
  // returned made the next request fail with HTTP 400 `input.content type
  // 'refusal' is not supported`. Every other role keeps that rejection, because
  // only assistant output produces a refusal part.
  if (type === "refusal" && role === "assistant") {
    if (typeof part.refusal !== "string") return failure("input.content.refusal", "input.content refusal must be a string");
    return { ok: true, value: { text: part.refusal } };
  }
  if (type !== "input_image") return failure("input.content.type", `input.content type '${type ?? "unknown"}' is not supported`);
  const url = getString(part.image_url) ?? getString(part.file_url);
  if (!url) return failure("input.content.image_url", "input_image requires image_url");
  const detail = getString(part.detail);
  return { ok: true, value: { image: { type: "image_url", image_url: { url, ...(detail ? { detail } : {}) } } } };
};

/** Chat Completions content parts are strings or image parts; Responses nests text. */
const chatContentFromResponseParts = (value: unknown, role: string): DeepSeekResponsesResult<string | ChatContentPart[]> => {
  if (typeof value === "string") return { ok: true, value };
  if (!Array.isArray(value)) return failure("input.content", "input.content must be a string or an array");
  const images: ChatContentPart[] = [];
  const texts: string[] = [];
  for (const raw of value) {
    const part = chatContentPart(raw, role);
    if (!part.ok) return part;
    if (part.value.text !== undefined) texts.push(part.value.text);
    if (part.value.image) images.push(part.value.image);
  }
  // Collapse a text-only message to the plain string form the provider expects.
  if (!images.length) return { ok: true, value: texts.join("") };
  if (texts.length) images.unshift({ type: "text", text: texts.join("") });
  return { ok: true, value: images };
};

const chatToolCallItem = (item: Record<string, unknown>): DeepSeekResponsesResult<Record<string, unknown>> => {
  const callId = getString(item.call_id) ?? getString(item.id);
  const name = getString(item.name);
  if (!callId || !name) return failure("input", "function_call items require call_id and name");
  const args = typeof item.arguments === "string" ? item.arguments : "{}";
  return { ok: true, value: { id: callId, type: "function", function: { name, arguments: args } } };
};

const chatToolResultItem = (item: Record<string, unknown>): DeepSeekResponsesResult<Record<string, unknown>> => {
  const callId = getString(item.call_id);
  if (!callId) return failure("input", "function_call_output items require call_id");
  const content = typeof item.output === "string" ? item.output : JSON.stringify(item.output ?? "");
  return { ok: true, value: { role: "tool", tool_call_id: callId, content } };
};

/**
 * A freeform (`custom`) tool call replayed from history. Chat Completions has no
 * custom tool type, so the freeform text travels as the single `input` argument
 * the adapter advertises for those tools, wrapped as the JSON string Chat
 * requires. Without this translation Codex rejected the whole request:
 * "input item type 'custom_tool_call' is not supported".
 */
const chatCustomToolCallItem = (item: Record<string, unknown>): DeepSeekResponsesResult<Record<string, unknown>> => {
  const callId = getString(item.call_id) ?? getString(item.id);
  const name = getString(item.name);
  if (!callId || !name) return failure("input", "custom_tool_call items require call_id and name");
  const input = typeof item.input === "string" ? item.input : "";
  return { ok: true, value: { id: callId, type: "function", function: { name, arguments: JSON.stringify({ input }) } } };
};

/**
 * The chain-of-thought text a `reasoning` input item carries. Codex echoes the
 * reasoning item this adapter emitted, either as `summary` parts or as
 * `content` parts depending on the client version.
 */
const reasoningTextFromItem = (item: Record<string, unknown>): string => {
  const parts = [item.summary, item.content].filter(Array.isArray).flat();
  const texts = parts.map((part) => (isRecord(part) ? getString(part.text) : null)).filter((text): text is string => Boolean(text));
  const direct = getString(item.text);
  if (direct) texts.push(direct);
  return texts.join("");
};

/** Appends one `function_call` item, merging consecutive calls into one assistant turn. */
const appendToolCall = (messages: Record<string, unknown>[], call: Record<string, unknown>, pending: { reasoning: string }): void => {
  const previous = messages.at(-1);
  if (previous?.role === "assistant" && Array.isArray(previous.tool_calls) && previous.content === null) {
    previous.tool_calls.push(call);
    return;
  }
  const turn: Record<string, unknown> = { role: "assistant", content: null, tool_calls: [call] };
  if (pending.reasoning) {
    turn.reasoning_content = pending.reasoning;
    pending.reasoning = "";
  }
  messages.push(turn);
};

const appendMessageItem = (
  messages: Record<string, unknown>[],
  item: Record<string, unknown>,
  pending: { reasoning: string }
): DeepSeekResponsesResult<void> => {
  const role = getString(item.role) ?? "user";
  if (role !== "user" && role !== "assistant" && role !== "developer") return failure("input.role", `input role '${role}' is not supported`);
  const content = chatContentFromResponseParts(item.content, role);
  if (!content.ok) return content;
  const message: Record<string, unknown> = { role: role === "developer" ? "system" : role, content: content.value };
  if (role === "assistant" && pending.reasoning) {
    message.reasoning_content = pending.reasoning;
    pending.reasoning = "";
  }
  // DeepSeek, like OpenAI Chat, has no developer role.
  messages.push(message);
  return { ok: true, value: undefined };
};

/**
 * Converts one Responses input array into Chat Completions messages. A
 * `function_call` item and its matching `function_call_output` become an
 * assistant turn carrying `tool_calls` followed by the tool result, which is
 * the only shape the Chat contract accepts. A `reasoning` item is carried onto
 * the assistant turn that follows it as `reasoning_content`.
 */
const appendInputItem = (messages: Record<string, unknown>[], rawItem: unknown, pending: { reasoning: string }): DeepSeekResponsesResult<void> => {
  if (typeof rawItem === "string") {
    messages.push({ role: "user", content: rawItem });
    return { ok: true, value: undefined };
  }
  if (!isRecord(rawItem) || Array.isArray(rawItem)) return failure("input", "input items must be objects");
  const type = getString(rawItem.type) ?? "message";
  if (type === "reasoning") {
    // Held for the assistant turn that follows it rather than dropped.
    pending.reasoning += reasoningTextFromItem(rawItem);
    return { ok: true, value: undefined };
  }
  // A freeform (`custom`) call carries raw text and a function call carries JSON
  // arguments; both become one assistant `tool_calls` entry either way.
  if (type === "function_call" || type === "custom_tool_call") {
    const call = type === "custom_tool_call" ? chatCustomToolCallItem(rawItem) : chatToolCallItem(rawItem);
    if (!call.ok) return call;
    appendToolCall(messages, call.value, pending);
    return { ok: true, value: undefined };
  }
  if (type === "function_call_output" || type === "custom_tool_call_output") {
    const result = chatToolResultItem(rawItem);
    if (!result.ok) return result;
    messages.push(result.value);
    return { ok: true, value: undefined };
  }
  if (type !== "message") return failure("input.type", `input item type '${type}' is not supported`);
  return appendMessageItem(messages, rawItem, pending);
};

export const toDeepSeekChatMessages = (input: unknown, instructions: string | null): DeepSeekResponsesResult<Record<string, unknown>[]> => {
  const messages: Record<string, unknown>[] = [];
  if (instructions) messages.push({ role: "system", content: instructions });
  if (typeof input === "string") {
    if (input) messages.push({ role: "user", content: input });
    return { ok: true, value: messages };
  }
  if (input === undefined || input === null) return { ok: true, value: messages };
  if (!Array.isArray(input)) return failure("input", "input must be a string or an array");

  const pending = { reasoning: "" };
  for (const rawItem of input) {
    const appended = appendInputItem(messages, rawItem, pending);
    if (!appended.ok) return appended;
  }
  return { ok: true, value: messages };
};

/**
 * DeepSeek rejects a tool-bearing request whose assistant turns omit
 * `reasoning_content` while thinking mode is on:
 *
 *   "The `reasoning_content` in the thinking mode must be passed back to the API."
 *
 * The measured boundary (probed against `deepseek-v4-flash`, `reasoning_effort`
 * other than `none`, with tools advertised) is: every assistant message that
 * follows the last `user` message must carry the field, and an empty string
 * satisfies it. Messages before the last user message are exempt, which is why
 * the fill is restricted to that tail instead of touching replayed history.
 *
 * A tool-call turn is not the only shape that trips this: Codex echoes the
 * assistant's text message ahead of its `function_call`, so the request that
 * continues a tool call carries a plain assistant message in the tail as well.
 * Filling only `tool_calls` turns left that message bare and the follow-up died
 * with HTTP 400 (observed in production on `deepseek-v4-flash`, `max`).
 */
const ensureTrailingAssistantReasoning = (messages: readonly Record<string, unknown>[]): void => {
  const lastUser = messages.findLastIndex((message) => message.role === "user");
  for (const message of messages.slice(lastUser + 1)) {
    if (message.role !== "assistant") continue;
    if (typeof message.reasoning_content !== "string") message.reasoning_content = "";
  }
};

/**
 * Collects Chat functions, flattening Responses `namespace` groups.
 *
 * Responses tool entries are flat and Codex groups related functions under a
 * `namespace` entry; Chat Completions has neither shape. A flattened name must
 * stay inside DeepSeek's `[a-zA-Z0-9_-]{1,128}` function name rule (a dot is
 * not accepted), so a collision takes its namespace as a prefix and is recorded
 * for reverse translation of the returned call. Tool types the official API
 * cannot serve (`web_search`, and anything else non-function) are dropped
 * rather than advertised.
 */
const chatFunctionRecord = (fn: Record<string, unknown>, chatName: string): Record<string, unknown> => ({
  type: "function",
  function: {
    name: chatName,
    ...(typeof fn.description === "string" ? { description: fn.description } : {}),
    ...(fn.parameters === undefined ? {} : { parameters: fn.parameters }),
    ...(typeof fn.strict === "boolean" ? { strict: fn.strict } : {}),
  },
});

/** Names must stay distinct and rule-compliant, so collisions suffix deterministically. */
const uniqueChatName = (used: ReadonlySet<string>, name: string): string => {
  if (!used.has(name)) return name;
  for (let suffix = 2; ; suffix += 1) {
    const candidate = `${name}_${suffix}`;
    if (!used.has(candidate)) return candidate;
  }
};

type ToolCollector = { tools: Record<string, unknown>[]; toolNames: Map<string, string>; customNames: Set<string>; used: Set<string> };

/**
 * The one parameter a freeform tool is advertised with. Codex's `apply_patch`
 * and code-mode `exec` tools take raw text, and Chat Completions only has JSON
 * function arguments, so the adapter asks for that text under `input` and
 * unwraps it again on the way back.
 */
const CUSTOM_TOOL_PARAMETERS = {
  type: "object",
  properties: { input: { type: "string", description: "Freeform input for the tool." } },
  required: ["input"],
  additionalProperties: false,
};

const collectCustom = (collector: ToolCollector, tool: Record<string, unknown>): DeepSeekResponsesResult<void> => {
  const name = getString(tool.name);
  if (!name) return failure("tools.name", "custom tools require a name");
  const chatName = uniqueChatName(collector.used, name);
  collector.used.add(chatName);
  collector.customNames.add(chatName);
  if (chatName !== name) collector.toolNames.set(chatName, name);
  collector.tools.push({
    type: "function",
    function: {
      name: chatName,
      ...(typeof tool.description === "string" ? { description: tool.description } : {}),
      parameters: CUSTOM_TOOL_PARAMETERS,
    },
  });
  return { ok: true, value: undefined };
};

const collectFunction = (collector: ToolCollector, fn: Record<string, unknown>, namespace: string | null): DeepSeekResponsesResult<void> => {
  const name = getString(fn.name);
  if (!name) return failure("tools.name", "function tools require a name");
  const preferred = namespace !== null && collector.used.has(name) ? `${namespace}_${name}` : name;
  const chatName = uniqueChatName(collector.used, preferred);
  collector.used.add(chatName);
  if (chatName !== name) collector.toolNames.set(chatName, name);
  collector.tools.push(chatFunctionRecord(fn, chatName));
  return { ok: true, value: undefined };
};

const collectNamespace = (collector: ToolCollector, tool: Record<string, unknown>): DeepSeekResponsesResult<void> => {
  if (!Array.isArray(tool.tools)) return failure("tools.tools", "namespace tools must nest a tools array");
  const namespace = getString(tool.name);
  for (const nested of tool.tools) {
    if (!isRecord(nested) || Array.isArray(nested)) return failure("tools.tools", "namespace tools must contain objects");
    // A namespace may only group functions; a nested non-function is dropped.
    if (getString(nested.type) !== "function") continue;
    const added = collectFunction(collector, nested, namespace);
    if (!added.ok) return added;
  }
  return { ok: true, value: undefined };
};

const collectTool = (collector: ToolCollector, tool: unknown): DeepSeekResponsesResult<void> => {
  if (!isRecord(tool) || Array.isArray(tool)) return failure("tools", "tools must contain objects");
  const type = getString(tool.type);
  if (type === "namespace") return collectNamespace(collector, tool);
  // A freeform (`custom`) tool is advertised as a function taking one string so
  // the model can still invoke it, and its call is translated back into a
  // `custom_tool_call` item the client recognizes.
  if (type === "custom") return collectCustom(collector, tool);
  const nested = isRecord(tool.function) && !Array.isArray(tool.function) ? tool.function : null;
  // A nested function object is always a function; otherwise only an explicit
  // `function` type is translatable and anything else (for example
  // `web_search`) is dropped.
  if (!nested && type !== "function") return { ok: true, value: undefined };
  return collectFunction(collector, nested ?? tool, null);
};

const toChatTools = (
  value: unknown
): DeepSeekResponsesResult<Readonly<{ tools: Record<string, unknown>[]; toolNames: ReadonlyMap<string, string>; customToolNames: ReadonlySet<string> }>> => {
  if (!Array.isArray(value)) return failure("tools", "tools must be an array");
  const collector: ToolCollector = { tools: [], toolNames: new Map(), customNames: new Set(), used: new Set() };
  for (const tool of value) {
    const collected = collectTool(collector, tool);
    if (!collected.ok) return collected;
  }
  return { ok: true, value: { tools: collector.tools, toolNames: collector.toolNames, customToolNames: collector.customNames } };
};

const toChatToolChoice = (value: unknown, toolNames: ReadonlyMap<string, string>): DeepSeekResponsesResult<unknown> => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (typeof value === "string") {
    if (value === "none" || value === "auto" || value === "required") return { ok: true, value };
    return failure("tool_choice", `tool_choice '${value}' is not supported`);
  }
  if (!isRecord(value) || Array.isArray(value)) return failure("tool_choice", "tool_choice must be a string or an object");
  const name = getString(value.name) ?? (isRecord(value.function) ? getString(value.function.name) : null);
  if (!name) return failure("tool_choice.name", "a named tool_choice requires a name");
  return { ok: true, value: { type: "function", function: { name: originalToolName(name, toolNames) } } };
};

/** Responses nests the output format under `text.format`; Chat has one flat field. */
const toChatResponseFormat = (value: unknown): DeepSeekResponsesResult<Record<string, unknown> | undefined> => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (!isRecord(value) || Array.isArray(value)) return failure("text", "text must be an object");
  const format = value.format;
  if (format === undefined || format === null) return { ok: true, value: undefined };
  if (!isRecord(format) || Array.isArray(format)) return failure("text.format", "text.format must be an object");
  const type = getString(format.type) ?? "text";
  if (type === "text") return { ok: true, value: undefined };
  if (type === "json_object") return { ok: true, value: { type: "json_object" } };
  return failure("text.format.type", `text.format type '${type}' is not supported upstream`);
};

const positiveInteger = (value: unknown): number | null => (typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null);

const applyOutputLimit = (body: Record<string, unknown>, rawRecord: Record<string, unknown>): DeepSeekResponsesResult<void> => {
  const raw = rawRecord.max_output_tokens;
  if (raw === undefined || raw === null) return { ok: true, value: undefined };
  const maxTokens = positiveInteger(raw);
  if (maxTokens === null) return failure("max_output_tokens", "max_output_tokens must be a positive integer");
  body.max_tokens = maxTokens;
  return { ok: true, value: undefined };
};

const applyReasoning = (
  body: Record<string, unknown>,
  rawRecord: Record<string, unknown>,
  profile: ChatOnlyResponsesProfile
): DeepSeekResponsesResult<void> => {
  const raw = rawRecord.reasoning;
  if (raw === undefined || raw === null) return { ok: true, value: undefined };
  if (!isRecord(raw) || Array.isArray(raw)) return failure("reasoning", "reasoning must be an object");
  const effort = getString(raw.effort);
  if (!effort) return { ok: true, value: undefined };
  const wireEffort = profile.projectReasoningEffort(effort);
  if (wireEffort === null) return failure("reasoning.effort", `reasoning.effort '${effort}' is not supported by ${profile.label}`);
  body.reasoning_effort = wireEffort;
  return { ok: true, value: undefined };
};

const applyTools = (
  body: Record<string, unknown>,
  rawRecord: Record<string, unknown>,
  profile: ChatOnlyResponsesProfile
): DeepSeekResponsesResult<Readonly<{ toolNames: ReadonlyMap<string, string>; customToolNames: ReadonlySet<string> }>> => {
  const toolNames = new Map<string, string>();
  let customToolNames: ReadonlySet<string> = new Set();
  if (rawRecord.tools !== undefined) {
    const tools = toChatTools(rawRecord.tools);
    if (!tools.ok) return tools;
    if (tools.value.tools.length) body.tools = tools.value.tools;
    for (const [chatName, original] of tools.value.toolNames) toolNames.set(chatName, original);
    customToolNames = tools.value.customToolNames;
  }
  const toolChoice = toChatToolChoice(rawRecord.tool_choice, toolNames);
  if (!toolChoice.ok) return toolChoice;
  if (toolChoice.value !== undefined) {
    // Thinking mode rejects `required` and the named-function form with an
    // upstream 400 (`Thinking mode does not support this tool_choice`). Reject
    // the incompatible combination here so the client gets a gateway-shaped
    // error naming both fields instead of the provider's message about a
    // parameter it believes it supports, and so the request never leaves the
    // gateway only to fail upstream. Probed 2026-09-21 on the Chat endpoint and
    // on the provider's native Responses endpoint; both reject it.
    const conflict = profile.thinkingToolChoiceConflict(body.reasoning_effort, rawRecord.thinking, toolChoice.value);
    if (conflict) return failure("tool_choice", profile.toolChoiceThinkingConflictMessage(conflict, "reasoning.effort"));
    body.tool_choice = toolChoice.value;
  }
  if (typeof rawRecord.parallel_tool_calls === "boolean") body.parallel_tool_calls = rawRecord.parallel_tool_calls;
  const responseFormat = toChatResponseFormat(rawRecord.text);
  if (!responseFormat.ok) return responseFormat;
  if (responseFormat.value) body.response_format = responseFormat.value;
  return { ok: true, value: { toolNames, customToolNames } };
};

/**
 * Codex can end a turn after a progress-only assistant message while the
 * requested action is still outstanding. This reminder keeps the client's
 * executable tools in play: it extends the caller's instructions (or becomes
 * the system message when the caller sent none) only for tool-bearing requests
 * that permit tool use. It never forces a tool call, forbids a legitimate final
 * answer, or claims any tool action was performed.
 */
const CONTINUATION_INSTRUCTION =
  "When tools are available, a progress update does not complete a requested action. If required work remains and you can perform it, continue with the next appropriate tool call instead of ending with a status message. Provide a final answer when the requested work is complete, or when you need user input or are blocked. Never claim a tool action has been done unless its result is in the conversation.";

const appendContinuationInstruction = (messages: Record<string, unknown>[]): void => {
  const system = messages.find((message) => message.role === "system" && typeof message.content === "string");
  if (system) system.content = `${String(system.content)}\n\n${CONTINUATION_INSTRUCTION}`;
  else messages.unshift({ role: "system", content: CONTINUATION_INSTRUCTION });
};

/**
 * Builds the Chat Completions body for a Responses request under one provider
 * profile. Translation failures are returned so the caller can answer with a
 * precise `invalid_request_error` instead of dispatching an approximation.
 */
export const toDeepSeekResponsesChatBody = (
  rawRecord: Record<string, unknown>,
  requestedModel: string,
  clientWantsStream: boolean,
  profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE
): DeepSeekResponsesResult<Readonly<{ body: Record<string, unknown>; toolNames: ReadonlyMap<string, string>; customToolNames: ReadonlySet<string> }>> => {
  const canonical = profile.upstreamModelFor(requestedModel);
  if (!canonical) return failure("model", `model '${requestedModel}' is not a ${profile.label} official model`);
  const instructions = typeof rawRecord.instructions === "string" && rawRecord.instructions.trim() ? rawRecord.instructions : null;
  const messages = toDeepSeekChatMessages(rawRecord.input, instructions);
  if (!messages.ok) return messages;
  if (!messages.value.length) return failure("input", "input must contain at least one message");

  const body: Record<string, unknown> = { model: canonical, messages: messages.value, stream: clientWantsStream };
  // A provider that reports streaming usage only when asked requires the option
  // beside `stream`; one that reports it unconditionally must not be sent it.
  if (clientWantsStream && profile.requiresStreamUsageOption) body.stream_options = { include_usage: true };
  const outputLimit = applyOutputLimit(body, rawRecord);
  if (!outputLimit.ok) return outputLimit;
  const reasoning = applyReasoning(body, rawRecord, profile);
  if (!reasoning.ok) return reasoning;
  const toolNames = applyTools(body, rawRecord, profile);
  if (!toolNames.ok) return toolNames;
  // Only a tool-bearing request makes the provider require replayed reasoning.
  if (Array.isArray(body.tools) && body.tools.length) {
    // `tool_choice: "none"` stays a hard no-tools request, and a request without
    // mapped executable tools (ordinary non-agent traffic) is untouched.
    if (rawRecord.tool_choice !== "none") appendContinuationInstruction(messages.value);
    ensureTrailingAssistantReasoning(messages.value);
  }
  return { ok: true, value: { body, toolNames: toolNames.value.toolNames, customToolNames: toolNames.value.customToolNames } };
};

/**
 * Message content parts in Responses order: the answer text, then a refusal.
 * A refusal is answer-bearing payload on this route's own Chat contract
 * (`CompletionAnswerBearingOutput.refusal`), so it is carried as the schema's
 * `refusal` part instead of being dropped.
 */
