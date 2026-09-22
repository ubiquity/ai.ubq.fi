import {
  deepSeekCachedPromptTokens,
  type DeepSeekFinishDisposition,
  deepSeekFinishDisposition,
  deepSeekReasoningTokens,
  deepSeekThinkingToolChoiceConflict,
  deepSeekToolChoiceThinkingConflictMessage,
  deepSeekUpstreamModelFor,
  projectDeepSeekReasoningEffort,
} from "./deepseek.ts";
import { getString, isRecord } from "./utils.ts";

/**
 * Responses <-> DeepSeek Chat Completions adapter.
 *
 * The Codex CLI speaks only the Responses API (`wire_api = "chat"` was removed
 * from the client), so this module translates a Responses request into a Chat
 * Completions body, a Chat completion back into a Responses object, and a Chat
 * SSE stream into the Responses event sequence the client consumes.
 *
 * The translation is not required by a provider gap: DeepSeek now serves a
 * native Responses endpoint (`POST /responses` and `/v1/responses`, probed
 * 2026-09-21). It stays the right seam because the provider's native endpoint is
 * documented as stateless with several control parameters ignored, and because
 * the `reasoning_content` fill this adapter applies to tool-bearing tails is a
 * measured provider requirement a native response would have to reproduce. A
 * migration is a separate evidence-driven evaluation, not an assumed cure.
 *
 * Only the gateway-known subset is translated. Anything else fails closed with
 * an `invalid_request_error` rather than being forwarded as an approximation.
 */

export type DeepSeekResponsesFailure = Readonly<{ ok: false; message: string; param: string }>;

export type DeepSeekResponsesResult<T> = Readonly<{ ok: true; value: T }> | DeepSeekResponsesFailure;

const failure = (param: string, message: string): DeepSeekResponsesFailure => ({ ok: false, message, param });

type ChatContentPart = Record<string, unknown>;

/** One Responses content part mapped onto the Chat content union. */
const chatContentPart = (part: unknown): DeepSeekResponsesResult<Readonly<{ text?: string; image?: ChatContentPart }>> => {
  if (!isRecord(part) || Array.isArray(part)) return failure("input.content", "input.content items must be objects");
  const type = getString(part.type);
  if (type === "input_text" || type === "output_text" || type === "text") {
    if (typeof part.text !== "string") return failure("input.content.text", "input.content text must be a string");
    return { ok: true, value: { text: part.text } };
  }
  if (type !== "input_image") return failure("input.content.type", `input.content type '${type ?? "unknown"}' is not supported`);
  const url = getString(part.image_url) ?? getString(part.file_url);
  if (!url) return failure("input.content.image_url", "input_image requires image_url");
  const detail = getString(part.detail);
  return { ok: true, value: { image: { type: "image_url", image_url: { url, ...(detail ? { detail } : {}) } } } };
};

/** Chat Completions content parts are strings or image parts; Responses nests text. */
const chatContentFromResponseParts = (value: unknown): DeepSeekResponsesResult<string | ChatContentPart[]> => {
  if (typeof value === "string") return { ok: true, value };
  if (!Array.isArray(value)) return failure("input.content", "input.content must be a string or an array");
  const images: ChatContentPart[] = [];
  const texts: string[] = [];
  for (const raw of value) {
    const part = chatContentPart(raw);
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
  const content = chatContentFromResponseParts(item.content);
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

/** Maps a flattened Chat tool name back to the name the client asked for. */
const originalToolName = (name: string, toolNames: ReadonlyMap<string, string>): string => toolNames.get(name) ?? name;

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

const applyReasoning = (body: Record<string, unknown>, rawRecord: Record<string, unknown>): DeepSeekResponsesResult<void> => {
  const raw = rawRecord.reasoning;
  if (raw === undefined || raw === null) return { ok: true, value: undefined };
  if (!isRecord(raw) || Array.isArray(raw)) return failure("reasoning", "reasoning must be an object");
  const effort = getString(raw.effort);
  if (effort) body.reasoning_effort = projectDeepSeekReasoningEffort(effort);
  return { ok: true, value: undefined };
};

const applyTools = (
  body: Record<string, unknown>,
  rawRecord: Record<string, unknown>
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
    const conflict = deepSeekThinkingToolChoiceConflict(body.reasoning_effort, rawRecord.thinking, toolChoice.value);
    if (conflict) return failure("tool_choice", deepSeekToolChoiceThinkingConflictMessage(conflict, "reasoning.effort"));
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
 * Builds the Chat Completions body for a Responses request. Translation
 * failures are returned so the caller can answer with a precise
 * `invalid_request_error` instead of dispatching an approximation.
 */
export const toDeepSeekResponsesChatBody = (
  rawRecord: Record<string, unknown>,
  requestedModel: string,
  clientWantsStream: boolean
): DeepSeekResponsesResult<Readonly<{ body: Record<string, unknown>; toolNames: ReadonlyMap<string, string>; customToolNames: ReadonlySet<string> }>> => {
  const canonical = deepSeekUpstreamModelFor(requestedModel);
  if (!canonical) return failure("model", `model '${requestedModel}' is not a DeepSeek official model`);
  const instructions = typeof rawRecord.instructions === "string" && rawRecord.instructions.trim() ? rawRecord.instructions : null;
  const messages = toDeepSeekChatMessages(rawRecord.input, instructions);
  if (!messages.ok) return messages;
  if (!messages.value.length) return failure("input", "input must contain at least one message");

  const body: Record<string, unknown> = { model: canonical, messages: messages.value, stream: clientWantsStream };
  // DeepSeek requires stream_options alongside a stream and reports usage on its
  // final content chunk rather than a separate frame.
  if (clientWantsStream) body.stream_options = { include_usage: true };
  const outputLimit = applyOutputLimit(body, rawRecord);
  if (!outputLimit.ok) return outputLimit;
  const reasoning = applyReasoning(body, rawRecord);
  if (!reasoning.ok) return reasoning;
  const toolNames = applyTools(body, rawRecord);
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
const responseMessageContent = (text: string, refusal: string): Record<string, unknown>[] => [
  ...(text ? [{ type: "output_text", text, annotations: [] }] : []),
  ...(refusal ? [{ type: "refusal", refusal }] : []),
];

const responseMessageItem = (id: string, text: string, refusal = ""): Record<string, unknown> => ({
  id,
  type: "message",
  status: "completed",
  role: "assistant",
  content: responseMessageContent(text, refusal),
});

const reasoningItem = (id: string, text: string): Record<string, unknown> => ({
  id,
  type: "reasoning",
  status: "completed",
  summary: [{ type: "summary_text", text }],
});

const functionCallItem = (id: string, callId: string, name: string, args: string): Record<string, unknown> => ({
  id,
  type: "function_call",
  status: "completed",
  call_id: callId,
  name,
  arguments: args,
});

/** Codex's freeform tool item; the text travels in `input`, not JSON arguments. */
const customToolCallItem = (id: string, callId: string, name: string, input: string): Record<string, unknown> => ({
  id,
  type: "custom_tool_call",
  status: "completed",
  call_id: callId,
  name,
  input,
});

/**
 * Unwraps the single freeform `input` parameter back into the tool's raw text.
 * The provider answers with JSON arguments because Chat Completions has no
 * freeform tool shape, and the client expects the original text.
 */
const freeformInputFromArguments = (args: string): string => {
  try {
    const parsed: unknown = JSON.parse(args);
    if (typeof parsed === "string") return parsed;
    if (isRecord(parsed) && !Array.isArray(parsed)) {
      const input = getString(parsed.input);
      if (input !== null) return input;
    }
  } catch {
    // Not JSON: the arguments already are the freeform input.
  }
  return args;
};

/**
 * Maps one Chat Completions usage object onto the Responses usage shape.
 *
 * The counters are the reason this is not a field-by-field copy: Codex reads
 * `input_tokens_details.cached_tokens` and `output_tokens_details.
 * reasoning_tokens`, and DeepSeek publishes those measurements as
 * `prompt_cache_hit_tokens` and `completion_tokens_details.reasoning_tokens`.
 * A counter the upstream did not report leaves its detail object absent, so the
 * client and the gateway telemetry both read an unknown value instead of a
 * measured zero.
 */
export const toResponsesUsage = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  const inputTokens = typeof value.prompt_tokens === "number" ? value.prompt_tokens : null;
  const outputTokens = typeof value.completion_tokens === "number" ? value.completion_tokens : null;
  if (inputTokens === null || outputTokens === null) return null;
  const totalTokens = typeof value.total_tokens === "number" ? value.total_tokens : inputTokens + outputTokens;
  const cachedTokens = deepSeekCachedPromptTokens(value, inputTokens);
  const reasoningTokens = deepSeekReasoningTokens(value, outputTokens);
  return {
    input_tokens: inputTokens,
    ...(cachedTokens === null ? {} : { input_tokens_details: { cached_tokens: cachedTokens } }),
    output_tokens: outputTokens,
    ...(reasoningTokens === null ? {} : { output_tokens_details: { reasoning_tokens: reasoningTokens } }),
    total_tokens: totalTokens,
  };
};

/**
 * The neutral instruction appended after the first-leg assistant draft when this
 * route performs its one bounded continuation recheck. It asks the same model
 * the same task a second time with the draft in context; it never forces a tool
 * call, forbids a legitimate final answer, or invents work.
 */
export const DEEPSEEK_RECHECK_INSTRUCTION =
  "The assistant response above is a draft for this same task. If actionable required work remains, emit the next appropriate tool call now. If the requested work is complete, blocked, or needs user input, return the existing answer without tool calls. Do not repeat completed actions or invent new work.";

/**
 * The most output one recheck may add. The caller's own remaining allowance is
 * the tighter bound whenever the first leg used fewer than this many tokens, so
 * the two legs together never exceed the allowance the caller asked for.
 */
export const DEEPSEEK_RECHECK_MAX_TOKENS = 8_192;

/** Why one first-leg completion did not earn the bounded recheck. */
export type DeepSeekRecheckSkipReason =
  "finish_reason" | "refusal" | "empty_text" | "tool_calls" | "no_executable_tools" | "tool_choice" | "usage_unknown" | "budget_unknown" | "budget_exhausted";

export type DeepSeekRecheckEligibility =
  Readonly<{ eligible: true; remainingBudget: number; maxTokens: number }> | Readonly<{ eligible: false; reason: DeepSeekRecheckSkipReason }>;

/**
 * Decides whether a first-leg completion earns the one bounded recheck.
 *
 * Only a clean `stop` that produced assistant text with no tool call and no
 * refusal is reconsidered, and only when the request advertises mapped
 * executable tools while leaving `tool_choice` absent or `auto`. The provider
 * must have reported the first leg's completion use, and the request must have a
 * known positive remaining output allowance, because a recheck with no measured
 * budget could double the caller's bill without a bound. A truncation, a
 * refusal, an empty answer, and a request that never permitted a tool stay
 * untouched.
 */
export const deepSeekRecheckEligibility = (
  input: Readonly<{
    finishReason: unknown;
    text: string;
    refusal: unknown;
    toolCallCount: number;
    executableToolCount: number;
    toolChoice: unknown;
    firstCompletionTokens: number | null;
    allowance: number | null;
  }>
): DeepSeekRecheckEligibility => {
  if (input.finishReason !== "stop") return { eligible: false, reason: "finish_reason" };
  if (typeof input.refusal === "string" && input.refusal.length > 0) return { eligible: false, reason: "refusal" };
  if (input.text.length === 0) return { eligible: false, reason: "empty_text" };
  if (input.toolCallCount > 0) return { eligible: false, reason: "tool_calls" };
  if (input.executableToolCount <= 0) return { eligible: false, reason: "no_executable_tools" };
  if (input.toolChoice !== undefined && input.toolChoice !== "auto") return { eligible: false, reason: "tool_choice" };
  if (input.firstCompletionTokens === null || !Number.isFinite(input.firstCompletionTokens)) {
    return { eligible: false, reason: "usage_unknown" };
  }
  if (input.allowance === null || !Number.isFinite(input.allowance)) return { eligible: false, reason: "budget_unknown" };
  const remainingBudget = input.allowance - input.firstCompletionTokens;
  if (remainingBudget <= 0) return { eligible: false, reason: "budget_exhausted" };
  return { eligible: true, remainingBudget, maxTokens: Math.min(remainingBudget, DEEPSEEK_RECHECK_MAX_TOKENS) };
};

/**
 * Builds the one buffered recheck request from the first-leg Chat body. It keeps
 * the same model, effort and original tool schemas, appends the first assistant
 * draft (including its reasoning, which the provider requires on a tool-bearing
 * tail) plus the neutral user instruction, and never adds a tool, an alias, a
 * schema field, or a forced `tool_choice`.
 */
export const toDeepSeekRecheckChatBody = (
  chatBody: Record<string, unknown>,
  draft: Readonly<{ content: string; reasoning: string }>,
  maxTokens: number
): Record<string, unknown> => {
  const messages = Array.isArray(chatBody.messages) ? [...chatBody.messages] : [];
  messages.push({ role: "assistant", content: draft.content, reasoning_content: draft.reasoning });
  messages.push({ role: "user", content: DEEPSEEK_RECHECK_INSTRUCTION });
  const recheck: Record<string, unknown> = { ...chatBody, messages, stream: false, max_tokens: maxTokens };
  // DeepSeek answers 400 when stream_options is present without stream:true.
  delete recheck.stream_options;
  return recheck;
};

/**
 * The executable tool calls one recheck returned, or null when it carries none.
 * Every call must name a tool this request advertised: a call the client never
 * offered is rejected as a batch rather than translated into a hidden tool or
 * alias the client cannot execute.
 */
export const deepSeekRecheckToolCalls = (message: Record<string, unknown>, executableNames: ReadonlySet<string>): Record<string, unknown>[] | null => {
  const toolCalls = Array.isArray(message.tool_calls) ? message.tool_calls : [];
  if (!toolCalls.length) return null;
  const calls: Record<string, unknown>[] = [];
  for (const call of toolCalls) {
    if (!isRecord(call) || Array.isArray(call)) return null;
    const fn = isRecord(call.function) && !Array.isArray(call.function) ? call.function : null;
    const name = fn ? getString(fn.name) : null;
    if (!name || !executableNames.has(name)) return null;
    calls.push(call);
  }
  return calls;
};

/** A measured token counter, or null when the leg did not report a usable amount. */
const validTokenCount = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) return null;
  return value;
};

/**
 * One aggregate base counter. Both legs' amounts are summed when both were
 * measured; otherwise the one measured amount is kept, because a leg that did
 * report tokens remains billable even when the other leg did not. `summed` is
 * false in that case so the caller never presents a two-request total.
 */
const mergedTokenCount = (firstValue: unknown, secondValue: unknown): Readonly<{ value: number | null; summed: boolean }> => {
  const first = validTokenCount(firstValue);
  const second = validTokenCount(secondValue);
  if (first !== null && second !== null) return { value: first + second, summed: true };
  if (first !== null) return { value: first, summed: false };
  if (second !== null) return { value: second, summed: false };
  return { value: null, summed: false };
};

const cachedTokensOf = (usage: Record<string, unknown>): unknown => {
  const details = isRecord(usage.prompt_tokens_details) && !Array.isArray(usage.prompt_tokens_details) ? usage.prompt_tokens_details : null;
  return usage.prompt_cache_hit_tokens ?? details?.cached_tokens;
};

const reasoningTokensOf = (usage: Record<string, unknown>): unknown => {
  const details = isRecord(usage.completion_tokens_details) && !Array.isArray(usage.completion_tokens_details) ? usage.completion_tokens_details : null;
  return details?.reasoning_tokens;
};

/**
 * The second leg's raw usage object when at least one base counter is
 * measurable, else null. Output validity is a separate question: a completion
 * the provider billed for keeps its measured counters even when its output is
 * rejected, while a payload with no measurable counter is not invented into one.
 */
export const measurableDeepSeekRecheckUsage = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  const measurable =
    validTokenCount(value.prompt_tokens) !== null || validTokenCount(value.completion_tokens) !== null || validTokenCount(value.total_tokens) !== null;
  return measurable ? value : null;
};

export type DeepSeekRecheckUsage = Readonly<{ usage: Record<string, unknown> | null; complete: boolean }>;

/**
 * The two-request total, held at or above the input and output counters both
 * legs independently observed: a second leg that reported only some counters
 * must not publish an internally inconsistent total. A total-only second
 * measurement is kept as reported even when it exceeds those partial components.
 */
const recheckReportedTotal = (total: number | null, observedComponentSum: number | null): number | null =>
  total !== null && observedComponentSum !== null && total < observedComponentSum ? observedComponentSum : total;

/**
 * Merges the usage of the two provider requests this route actually made.
 *
 * Every base counter keeps the amounts each leg independently measured: both
 * observed amounts are summed, and a counter only one leg reported keeps that
 * leg's measured value instead of being dropped or invented. The reported total
 * is never allowed below the input and output counters that were independently
 * observed, because a second leg that reported only some counters would
 * otherwise publish an internally inconsistent total; a total-only second
 * measurement is kept as reported even when it exceeds those partial components.
 * `complete` is true only when both legs measured every base counter and the
 * total is exactly their sum. Cache-read and reasoning details describe the
 * aggregate only when both legs measured them; otherwise they are omitted, so an
 * unknown detail is never published as a measured zero and a one-leg detail is
 * never presented as the two-request aggregate. A wholly unobserved second leg
 * keeps the first leg's base counters and adds no detail.
 */
export const mergeDeepSeekRecheckUsage = (first: unknown, second: unknown): DeepSeekRecheckUsage => {
  if (!isRecord(first) || Array.isArray(first)) return { usage: null, complete: false };
  const secondUsage = isRecord(second) && !Array.isArray(second) ? second : null;
  const promptTokens = mergedTokenCount(first.prompt_tokens, secondUsage?.prompt_tokens);
  const completionTokens = mergedTokenCount(first.completion_tokens, secondUsage?.completion_tokens);
  const totalTokens = mergedTokenCount(first.total_tokens, secondUsage?.total_tokens);
  const observedComponentSum = promptTokens.value !== null && completionTokens.value !== null ? promptTokens.value + completionTokens.value : null;
  const reportedTotal = recheckReportedTotal(totalTokens.value, observedComponentSum);
  const usage: Record<string, unknown> = {};
  if (promptTokens.value !== null) usage.prompt_tokens = promptTokens.value;
  if (completionTokens.value !== null) usage.completion_tokens = completionTokens.value;
  if (reportedTotal !== null) usage.total_tokens = reportedTotal;
  if (secondUsage) {
    const cachedTokens = mergedTokenCount(cachedTokensOf(first), cachedTokensOf(secondUsage));
    if (cachedTokens.summed) usage.prompt_tokens_details = { cached_tokens: cachedTokens.value };
    const reasoningTokens = mergedTokenCount(reasoningTokensOf(first), reasoningTokensOf(secondUsage));
    if (reasoningTokens.summed) usage.completion_tokens_details = { reasoning_tokens: reasoningTokens.value };
  }
  return { usage, complete: promptTokens.summed && completionTokens.summed && totalTokens.summed && reportedTotal === totalTokens.value };
};

export type DeepSeekResponsesEcho = Readonly<{
  tools: unknown;
  tool_choice: unknown;
  parallel_tool_calls: unknown;
  instructions: string | null;
}>;

/** Fields Codex reads back off a Responses object; the request is echoed verbatim. */
const responsesEnvelope = (
  responseId: string,
  requestedModel: string,
  createdAtSeconds: number,
  status: string,
  echo: DeepSeekResponsesEcho
): Record<string, unknown> => ({
  id: responseId,
  object: "response",
  created_at: createdAtSeconds,
  status,
  model: requestedModel,
  output: [],
  error: null,
  incomplete_details: null,
  instructions: echo.instructions,
  metadata: {},
  parallel_tool_calls: typeof echo.parallel_tool_calls === "boolean" ? echo.parallel_tool_calls : true,
  tool_choice: echo.tool_choice ?? "auto",
  tools: Array.isArray(echo.tools) ? echo.tools : [],
  temperature: null,
  top_p: null,
  max_output_tokens: null,
  previous_response_id: null,
  reasoning: null,
  store: false,
  truncation: "disabled",
  usage: null,
});

/**
 * The terminal event and status a DeepSeek `finish_reason` implies.
 *
 * The provider reports one vocabulary for both the Chat endpoint and its native
 * Responses endpoint; the official Responses schema defines only
 * `max_output_tokens`, `max_messages`, `content_filter` and `steered` as
 * `incomplete_details.reason` values, so the two incomplete-capable reasons map
 * onto the schema's own names. A resource interruption and an interruption of
 * unspecified cause are reported as a failed terminal rather than a clean
 * completion, because neither leaves a usable answer.
 */
export const deepSeekTerminalTypeForDisposition = (
  disposition: DeepSeekFinishDisposition
): "response.completed" | "response.incomplete" | "response.failed" => {
  if (disposition.kind === "completed") return "response.completed";
  if (disposition.kind === "incomplete") return "response.incomplete";
  return "response.failed";
};

/**
 * Builds the terminal Responses object for one DeepSeek finish disposition.
 * Only a completed disposition reports `completed`; an incomplete disposition
 * carries `incomplete_details.reason`, and every other disposition names the
 * provider's own reason as the error code so the cause stays visible.
 */
export const deepSeekTerminalEnvelope = (
  responseId: string,
  requestedModel: string,
  createdAtSeconds: number,
  status: string,
  echo: DeepSeekResponsesEcho,
  finishReason: unknown
): Readonly<{ type: "response.completed" | "response.incomplete" | "response.failed"; response: Record<string, unknown> }> => {
  const disposition = deepSeekFinishDisposition(finishReason);
  const terminalType = deepSeekTerminalTypeForDisposition(disposition);
  if (disposition.kind === "completed") {
    return { type: terminalType as "response.completed", response: responsesEnvelope(responseId, requestedModel, createdAtSeconds, status, echo) };
  }
  if (disposition.kind === "incomplete") {
    const response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "incomplete", echo);
    response.incomplete_details = { reason: disposition.reason };
    return { type: "response.incomplete", response };
  }
  const code = disposition.kind === "failed" ? disposition.code : `unrecognized_finish_reason:${disposition.value}`;
  const response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "failed", echo);
  response.error = { code, message: `DeepSeek stopped generating: ${code}` };
  return { type: "response.failed", response };
};

/** Output items for one Chat choice: reasoning, message, then any tool calls. */
const outputItemsForChoice = (
  message: Record<string, unknown>,
  choiceIndex: number,
  responseId: string,
  toolNames: ReadonlyMap<string, string>,
  customToolNames: ReadonlySet<string>
): Record<string, unknown>[] => {
  const items: Record<string, unknown>[] = [];
  if (typeof message.reasoning_content === "string" && message.reasoning_content) {
    items.push(reasoningItem(`${responseId}_rs_${choiceIndex}`, message.reasoning_content));
  }
  const text = typeof message.content === "string" ? message.content : "";
  const refusal = typeof message.refusal === "string" ? message.refusal : "";
  if (text || refusal) items.push(responseMessageItem(`${responseId}_msg_${choiceIndex}`, text, refusal));
  const toolCalls = Array.isArray(message.tool_calls) ? message.tool_calls : [];
  for (const [callIndex, call] of toolCalls.entries()) {
    if (!isRecord(call) || Array.isArray(call) || !isRecord(call.function) || Array.isArray(call.function)) continue;
    const callId = getString(call.id) ?? `${responseId}_call_${callIndex}`;
    const chatName = getString(call.function.name) ?? "";
    const name = originalToolName(chatName, toolNames);
    const args = typeof call.function.arguments === "string" ? call.function.arguments : "";
    if (customToolNames.has(chatName)) {
      items.push(customToolCallItem(`${responseId}_ctc_${choiceIndex}_${callIndex}`, callId, name, freeformInputFromArguments(args)));
      continue;
    }
    items.push(functionCallItem(`${responseId}_fc_${choiceIndex}_${callIndex}`, callId, name, args));
  }
  return items;
};

/**
 * Builds the buffered Responses object for a completed Chat completion. Chat
 * tool calls become `function_call` output items, and DeepSeek's
 * `reasoning_content` becomes a `reasoning` item so nothing is silently lost.
 */
export const toDeepSeekResponsesPayload = (
  completion: Record<string, unknown>,
  requestedModel: string,
  responseId: string,
  echo: DeepSeekResponsesEcho,
  toolNames: ReadonlyMap<string, string> = new Map(),
  customToolNames: ReadonlySet<string> = new Set()
): Record<string, unknown> => {
  const created = typeof completion.created === "number" ? completion.created : Math.floor(Date.now() / 1000);
  const choices = Array.isArray(completion.choices) ? completion.choices : [];
  // A buffered completion still carries `finish_reason`, so the same mapping
  // applies: a single-choice truncation must not be returned as a completed
  // response just because the transport delivered the whole body.
  const firstChoice = choices.find((choice) => isRecord(choice) && !Array.isArray(choice));
  const finishReason = isRecord(firstChoice) && !Array.isArray(firstChoice) ? firstChoice.finish_reason : undefined;
  const payload = deepSeekTerminalEnvelope(responseId, requestedModel, created, "completed", echo, finishReason).response;
  const output: Record<string, unknown>[] = [];
  for (const [index, choice] of choices.entries()) {
    if (!isRecord(choice) || Array.isArray(choice) || !isRecord(choice.message) || Array.isArray(choice.message)) continue;
    output.push(...outputItemsForChoice(choice.message, index, responseId, toolNames, customToolNames));
  }
  payload.output = output;
  payload.usage = toResponsesUsage(completion.usage);
  return payload;
};

type StreamToolCall = { id: string; callId: string; name: string; arguments: string; announced: boolean; outputIndex: number };

/**
 * The accumulated translated output facts a terminal-validity decision needs.
 *
 * Only the caller decides whether a completion is usable; this is the view it
 * decides on. Reasoning is deliberately absent: it is streaming progress, not
 * an answer a client can act on, so a stream whose only output is reasoning
 * still reports empty text and no tool calls here.
 */
export type DeepSeekResponsesAnswerBearingOutput = Readonly<{
  /** Assistant text accumulated from `delta.content`. */
  text: string;
  /** Refusal text accumulated from `delta.refusal`. */
  refusal: string;
  /** How many tool calls accumulated with a name the client can execute. */
  toolCallCount: number;
}>;

type StreamState = {
  started: boolean;
  completed: boolean;
  text: string;
  reasoning: string;
  refusal: string;
  messageIndex: number;
  messageOpen: boolean;
  messageDone: boolean;
  textPartIndex: number;
  refusalPartIndex: number;
  /**
   * The output slot reserved for the reasoning item. The streamed transport
   * never emits reasoning item events, so the slot is reserved at the first
   * reasoning delta and filled at the terminal. Without it the item would be
   * unshifted into position 0 and displace every item that does carry an
   * `output_index`.
   */
  reasoningIndex: number;
  toolCalls: Map<number, StreamToolCall>;
  nextOutputIndex: number;
  output: Record<string, unknown>[];
  usage: Record<string, unknown> | null;
  /**
   * The provider's own stop reason, captured rather than discarded. The
   * provider's client defers the mapped reason to its terminal sentinel so no
   * chunk follows it and usage always precedes it; this translator's `finish`
   * runs on the same sentinel, so the last non-null value wins.
   */
  finishReason: unknown;
};

const newStreamState = (): StreamState => ({
  started: false,
  completed: false,
  text: "",
  reasoning: "",
  refusal: "",
  messageIndex: -1,
  messageOpen: false,
  messageDone: false,
  textPartIndex: -1,
  refusalPartIndex: -1,
  reasoningIndex: -1,
  toolCalls: new Map(),
  nextOutputIndex: 0,
  output: [],
  usage: null,
  finishReason: undefined,
});

/**
 * The gateway terminal a Chat `finish_reason` maps onto, decided once here.
 *
 * `stop`, `tool_calls` and an absent reason are normal completions. `length`
 * means the provider stopped at an output or context boundary, which the
 * Responses schema reports as `incomplete` with the single reason
 * `max_output_tokens` (the schema spells it that way; it is not `max_tokens`).
 * Any other value must not be laundered into a normal stop: it becomes a
 * failed terminal, and the route records the raw value in telemetry.
 */
export type DeepSeekResponsesTerminalKind = "completed" | "incomplete" | "failed";

export const deepSeekResponsesTerminalKind = (finishReason: unknown): DeepSeekResponsesTerminalKind => {
  const disposition = deepSeekFinishDisposition(finishReason);
  if (disposition.kind === "completed") return "completed";
  if (disposition.kind === "incomplete") return "incomplete";
  return "failed";
};

/** Merges one tool-call delta into the accumulated call for its index. */
const mergeToolCallDelta = (state: StreamState, responseId: string, raw: Record<string, unknown>, position: number): StreamToolCall => {
  const key = typeof raw.index === "number" ? raw.index : position;
  const existing = state.toolCalls.get(key) ?? {
    id: `${responseId}_fc_${key}`,
    callId: getString(raw.id) ?? `${responseId}_call_${key}`,
    name: "",
    arguments: "",
    announced: false,
    outputIndex: -1,
  };
  const id = getString(raw.id);
  if (id) existing.callId = id;
  const fn = isRecord(raw.function) && !Array.isArray(raw.function) ? raw.function : null;
  const name = fn ? getString(fn.name) : null;
  if (name) existing.name = name;
  if (fn && typeof fn.arguments === "string") existing.arguments += fn.arguments;
  state.toolCalls.set(key, existing);
  return existing;
};

/**
 * Accumulates one Chat Completions SSE stream and emits the Responses event
 * sequence. The message item is announced lazily so a tool-only reply never
 * emits an empty text part.
 */
export const createDeepSeekResponsesStreamTranslator = (
  requestedModel: string,
  responseId: string,
  echo: DeepSeekResponsesEcho,
  createdAtSeconds: number,
  toolNames: ReadonlyMap<string, string> = new Map(),
  customToolNames: ReadonlySet<string> = new Set()
) => {
  const state = newStreamState();
  const messageId = `${responseId}_msg_0`;

  const startEvents = (): Record<string, unknown>[] => {
    if (state.started) return [];
    state.started = true;
    const created = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "in_progress", echo);
    return [
      { type: "response.created", response: created },
      { type: "response.in_progress", response: created },
    ];
  };

  const ensureMessageItem = (): Record<string, unknown>[] => {
    if (state.messageOpen) return [];
    state.messageOpen = true;
    state.messageIndex = state.nextOutputIndex++;
    return [
      {
        type: "response.output_item.added",
        output_index: state.messageIndex,
        item: { id: messageId, type: "message", status: "in_progress", role: "assistant", content: [] },
      },
    ];
  };

  /** Content parts are appended in arrival order, so their index is their position. */
  const nextContentIndex = (): number => (state.textPartIndex >= 0 ? 1 : 0) + (state.refusalPartIndex >= 0 ? 1 : 0);

  const addContentPart = (part: Record<string, unknown>, kind: "text" | "refusal"): Record<string, unknown>[] => {
    const contentIndex = nextContentIndex();
    if (kind === "text") state.textPartIndex = contentIndex;
    else state.refusalPartIndex = contentIndex;
    return [{ type: "response.content_part.added", item_id: messageId, output_index: state.messageIndex, content_index: contentIndex, part }];
  };

  const announceTextPart = (): Record<string, unknown>[] => [
    ...ensureMessageItem(),
    ...(state.textPartIndex >= 0 ? [] : addContentPart({ type: "output_text", text: "", annotations: [] }, "text")),
  ];

  /**
   * The terminal object for this stream, plus the terminal kind telemetry and
   * the caller report. Both come from the one shared DeepSeek reason mapping,
   * so the emitted event and its recorded classification cannot disagree.
   *
   * Scope note: this maps the provider's own reason vocabulary only. Whether an
   * answer-less completion is a valid completion is the provider-agnostic
   * completion-validity question, answered once by
   * `isAnswerBearingCompletion` and applied by the route caller.
   */
  const terminalEnvelope = () => deepSeekTerminalEnvelope(responseId, requestedModel, createdAtSeconds, "completed", echo, state.finishReason);

  const isCustomCall = (call: StreamToolCall): boolean => customToolNames.has(call.name);

  const announceToolCall = (call: StreamToolCall): Record<string, unknown>[] => {
    call.announced = true;
    call.outputIndex = state.nextOutputIndex++;
    const custom = isCustomCall(call);
    return [
      {
        type: "response.output_item.added",
        output_index: call.outputIndex,
        item: {
          id: call.id,
          type: custom ? "custom_tool_call" : "function_call",
          status: "in_progress",
          call_id: call.callId,
          name: originalToolName(call.name, toolNames),
          ...(custom ? { input: "" } : { arguments: "" }),
        },
      },
    ];
  };

  const applyTextDelta = (delta: Record<string, unknown>): Record<string, unknown>[] => {
    if (typeof delta.reasoning_content === "string" && delta.reasoning_content) {
      if (state.reasoningIndex < 0) state.reasoningIndex = state.nextOutputIndex++;
      state.reasoning += delta.reasoning_content;
    }
    if (typeof delta.content !== "string" || !delta.content) return [];
    state.text += delta.content;
    return [
      ...announceTextPart(),
      {
        type: "response.output_text.delta",
        item_id: messageId,
        output_index: state.messageIndex,
        content_index: state.textPartIndex,
        delta: delta.content,
        logprobs: [],
      },
    ];
  };

  /**
   * A refusal delta is answer-bearing payload, so it gets its own content part.
   * The event carries the fields this translator emits on every other streamed
   * event; `sequence_number` is a route-level concern (see the module handback),
   * not something only this event is missing.
   */
  const applyRefusalDelta = (delta: Record<string, unknown>): Record<string, unknown>[] => {
    if (typeof delta.refusal !== "string" || !delta.refusal) return [];
    state.refusal += delta.refusal;
    return [
      ...ensureMessageItem(),
      ...(state.refusalPartIndex >= 0 ? [] : addContentPart({ type: "refusal", refusal: "" }, "refusal")),
      {
        type: "response.refusal.delta",
        item_id: messageId,
        output_index: state.messageIndex,
        content_index: state.refusalPartIndex,
        delta: delta.refusal,
      },
    ];
  };

  const applyToolCallDeltas = (delta: Record<string, unknown>): Record<string, unknown>[] => {
    const events: Record<string, unknown>[] = [];
    const raw = Array.isArray(delta.tool_calls) ? delta.tool_calls : [];
    for (const [position, entry] of raw.entries()) {
      if (!isRecord(entry) || Array.isArray(entry)) continue;
      const call = mergeToolCallDelta(state, responseId, entry, position);
      if (!call.announced && call.name) events.push(...announceToolCall(call));
      const fn = isRecord(entry.function) && !Array.isArray(entry.function) ? entry.function : null;
      // A freeform call streams its input at the terminal item instead: the
      // provider sends JSON arguments, and the client wants the raw text.
      if (call.announced && !isCustomCall(call) && fn && typeof fn.arguments === "string" && fn.arguments) {
        events.push({ type: "response.function_call_arguments.delta", item_id: call.id, output_index: call.outputIndex, delta: fn.arguments });
      }
    }
    return events;
  };

  const closeMessage = (): Record<string, unknown>[] => {
    if (!state.messageOpen || state.messageDone) return [];
    state.messageDone = true;
    const events: Record<string, unknown>[] = [];
    if (state.textPartIndex >= 0) {
      events.push(
        {
          type: "response.output_text.done",
          item_id: messageId,
          output_index: state.messageIndex,
          content_index: state.textPartIndex,
          text: state.text,
          logprobs: [],
        },
        {
          type: "response.content_part.done",
          item_id: messageId,
          output_index: state.messageIndex,
          content_index: state.textPartIndex,
          part: { type: "output_text", text: state.text, annotations: [] },
        }
      );
    }
    if (state.refusalPartIndex >= 0) {
      events.push(
        {
          type: "response.refusal.done",
          item_id: messageId,
          output_index: state.messageIndex,
          content_index: state.refusalPartIndex,
          refusal: state.refusal,
        },
        {
          type: "response.content_part.done",
          item_id: messageId,
          output_index: state.messageIndex,
          content_index: state.refusalPartIndex,
          part: { type: "refusal", refusal: state.refusal },
        }
      );
    }
    const content: Record<string, unknown>[] = [];
    if (state.textPartIndex >= 0) content[state.textPartIndex] = { type: "output_text", text: state.text, annotations: [] };
    if (state.refusalPartIndex >= 0) content[state.refusalPartIndex] = { type: "refusal", refusal: state.refusal };
    const item: Record<string, unknown> = { id: messageId, type: "message", status: "completed", role: "assistant", content };
    state.output[state.messageIndex] = item;
    events.push({ type: "response.output_item.done", output_index: state.messageIndex, item });
    return events;
  };

  const closeToolCalls = (): Record<string, unknown>[] => {
    const events: Record<string, unknown>[] = [];
    const ordered = [...state.toolCalls.entries()].sort(([left], [right]) => left - right).map(([, call]) => call);
    for (const call of ordered) {
      if (!call.announced) {
        if (!call.name) continue;
        events.push(...announceToolCall(call));
      }
      if (isCustomCall(call)) {
        const input = freeformInputFromArguments(call.arguments);
        const item = customToolCallItem(call.id, call.callId, originalToolName(call.name, toolNames), input);
        if (input) {
          events.push({ type: "response.custom_tool_call_input.delta", item_id: call.id, output_index: call.outputIndex, call_id: call.callId, delta: input });
        }
        state.output[call.outputIndex] = item;
        events.push({ type: "response.output_item.done", output_index: call.outputIndex, item });
        continue;
      }
      events.push({ type: "response.function_call_arguments.done", item_id: call.id, output_index: call.outputIndex, arguments: call.arguments });
      const item = functionCallItem(call.id, call.callId, originalToolName(call.name, toolNames), call.arguments);
      state.output[call.outputIndex] = item;
      events.push({ type: "response.output_item.done", output_index: call.outputIndex, item });
    }
    return events;
  };

  return {
    /** Emits `response.created` / `response.in_progress` before any content. */
    open: startEvents,
    /** Translates one normalized Chat chunk into zero or more Responses events. */
    push: (chunk: Record<string, unknown>): Record<string, unknown>[] => {
      const events = startEvents();
      const usage = toResponsesUsage(chunk.usage);
      if (usage) state.usage = usage;
      const choices = Array.isArray(chunk.choices) ? chunk.choices : [];
      for (const choice of choices) {
        if (!isRecord(choice) || Array.isArray(choice)) continue;
        // `finish_reason` rides the final content chunk, which also carries the
        // usage statistics, and can arrive on a choice with no delta. Capture it
        // before the delta handling, which ignores the field; a later value
        // replaces an earlier one, so the last reason before the end sentinel
        // decides the terminal.
        if (typeof choice.finish_reason === "string") state.finishReason = choice.finish_reason;
        if (!isRecord(choice.delta) || Array.isArray(choice.delta)) continue;
        events.push(...applyTextDelta(choice.delta), ...applyRefusalDelta(choice.delta), ...applyToolCallDeltas(choice.delta));
      }
      return events;
    },
    /**
     * The accumulated output a client could act on: assistant text, a refusal,
     * or tool calls that carry a name. Reasoning is deliberately absent — it is
     * streaming progress, not an answer. This is the view the provider-agnostic
     * completion-validity predicate decides on, so a refusal is reported here
     * exactly as the Chat wire carries it and never fails closed as an empty
     * completion.
     */
    answerBearingOutput: (): DeepSeekResponsesAnswerBearingOutput => ({
      text: state.text,
      refusal: state.refusal,
      toolCallCount: [...state.toolCalls.values()].filter((call) => call.name).length,
    }),
    /**
     * How many tool-call deltas the stream observed at all, named or not. The
     * recheck contract is no tool call whatsoever, so eligibility reads this
     * count: an argument-only partial delta occupies a map slot a second
     * generation would otherwise concatenate onto.
     */
    observedToolCallCount: (): number => state.toolCalls.size,
    /**
     * The first-leg assistant draft the bounded recheck appends to the
     * conversation. Only the accumulated answer text and its reasoning are
     * exposed; the recheck's own text is never streamed back to the client.
     */
    draftAssistantMessage: (): Readonly<{ content: string; reasoning: string }> => ({ content: state.text, reasoning: state.reasoning }),
    /**
     * The terminal this stream will settle on, available before `finish` emits
     * it so telemetry records the same terminal the client receives. Derived
     * from the shared DeepSeek vocabulary mapping, so it cannot drift from the
     * event `finish` emits.
     */
    terminalKind: (): DeepSeekResponsesTerminalKind => deepSeekResponsesTerminalKind(state.finishReason),
    /** The raw recorded reason, for telemetry when the terminal is a failure. */
    upstreamFinishReason: (): string | null => (typeof state.finishReason === "string" ? state.finishReason : null),
    /**
     * Emits the remaining item events plus the terminal event the provider's
     * own stop reason implies. The terminal is derived at the one decision
     * point, so a truncated or interrupted generation cannot be reported as a
     * clean completion.
     */
    finish: (): Record<string, unknown>[] => {
      if (state.completed) return [];
      state.completed = true;
      // Items are stored at the position they were assigned an `output_index`
      // for, so `response.output[output_index]` is the item the client
      // accumulated at that index even when fragmented tool calls announced
      // their names out of call order.
      const events = [...startEvents(), ...closeMessage(), ...closeToolCalls()];
      if (state.reasoning) state.output[state.reasoningIndex] = reasoningItem(`${responseId}_rs_0`, state.reasoning);
      const terminal = terminalEnvelope();
      terminal.response.output = state.output;
      terminal.response.usage = state.usage;
      events.push({ type: terminal.type, response: terminal.response });
      return events;
    },
  };
};

/** Responses SSE frames are named events; the client dispatches on `event:`. */
export const encodeResponsesEvent = (value: Record<string, unknown>): string => `event: ${String(value.type)}\ndata: ${JSON.stringify(value)}\n\n`;
