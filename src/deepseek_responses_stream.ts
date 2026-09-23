// Chat SSE to Responses event stream translation, split out of src/deepseek_responses.ts.

import { type ChatOnlyResponsesProfile, DEEPSEEK_RESPONSES_PROFILE, originalToolName } from "./deepseek_responses.ts";
import {
  customToolCallItem,
  type DeepSeekResponsesEcho,
  deepSeekTerminalEnvelope,
  freeformInputFromArguments,
  functionCallItem,
  reasoningItem,
  responsesEnvelope,
  toResponsesUsage,
} from "./deepseek_responses_payload.ts";
import { getString, isRecord } from "./utils.ts";

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
   * The output slot the most recently announced reasoning item owns. The
   * provider's normal order opens it at the first reasoning delta, before any
   * later item advances the index; the item id is derived from that slot
   * (`${responseId}_rs_${reasoningIndex}`), matching the buffered transport's
   * first-choice name.
   */
  reasoningIndex: number;
  /**
   * True while an announced reasoning item is still awaiting its done events.
   * The pinned Codex consumer holds a single active item
   * (`lib/codex/codex-rs/core/src/session/turn.rs`), so this item is closed
   * before the next output item is announced.
   */
  reasoningOpen: boolean;
  /**
   * The text accumulated for the reasoning item that is currently open or
   * waiting for its terminal announcement. `reasoning` keeps the whole stream's
   * text for the first-leg draft.
   */
  reasoningSegment: string;
  /**
   * True when the current reasoning segment arrived after another output item
   * was already announced. Announcing it there would overlap that item for the
   * single-active-item consumer, so it is held and delivered as one closed
   * lifecycle at the terminal instead.
   */
  reasoningPending: boolean;
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
  reasoningOpen: false,
  reasoningSegment: "",
  reasoningPending: false,
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

export const deepSeekResponsesTerminalKind = (
  finishReason: unknown,
  profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE
): DeepSeekResponsesTerminalKind => {
  const disposition = profile.finishDisposition(finishReason);
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
 * sequence under one provider profile. The message item is announced lazily so
 * a tool-only reply never emits an empty text part.
 */
export const createDeepSeekResponsesStreamTranslator = (
  requestedModel: string,
  responseId: string,
  echo: DeepSeekResponsesEcho,
  createdAtSeconds: number,
  toolNames: ReadonlyMap<string, string> = new Map(),
  customToolNames: ReadonlySet<string> = new Set(),
  profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE
) => {
  const state = newStreamState();
  const messageId = `${responseId}_msg_0`;
  // The buffered transport names its first-choice reasoning item the same way
  // (`${responseId}_rs_${choiceIndex}` with choice index 0).
  const reasoningItemId = (): string => `${responseId}_rs_${state.reasoningIndex}`;

  /**
   * Closes the open reasoning item at the index it was announced at, using the
   * text accumulated for that item alone. It is called before the next output
   * item is announced and again at the terminal, so the pinned Codex consumer's
   * single `active_item` is always the item a done event closes. The terminal
   * item is stored at its own `output_index` and carries the same single
   * `summary_text` part the buffered transport publishes.
   */
  const closeReasoning = (): Record<string, unknown>[] => {
    if (!state.reasoningOpen) return [];
    state.reasoningOpen = false;
    const text = state.reasoningSegment;
    state.reasoningSegment = "";
    const itemId = reasoningItemId();
    const item = reasoningItem(itemId, text);
    state.output[state.reasoningIndex] = item;
    return [
      {
        type: "response.reasoning_summary_text.done",
        item_id: itemId,
        output_index: state.reasoningIndex,
        summary_index: 0,
        text,
      },
      {
        type: "response.reasoning_summary_part.done",
        item_id: itemId,
        output_index: state.reasoningIndex,
        summary_index: 0,
        part: { type: "summary_text", text },
      },
      { type: "response.output_item.done", output_index: state.reasoningIndex, item },
    ];
  };

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
    // The message is the next Codex-visible item, so the reasoning item must
    // finish first: the consumer clears its single active item on every done and
    // re-emits `ItemStarted` for a done that finds none.
    const events = closeReasoning();
    state.messageOpen = true;
    state.messageIndex = state.nextOutputIndex++;
    return [
      ...events,
      {
        type: "response.output_item.added",
        output_index: state.messageIndex,
        item: { id: messageId, type: "message", status: "in_progress", role: "assistant", content: [] },
      },
    ];
  };

  /**
   * Announces the reasoning item and its single summary part at the index the
   * item owns, before any later item can advance `nextOutputIndex`.
   *
   * The announcement is the contract the official client accumulates on:
   * `response.output_item.added` appends its item to the client's ordered output
   * and every later event reads that output at the `output_index` it names. A
   * slot reserved without this event (the previous behavior) made the next item
   * the client's first accumulated item while its content events still named a
   * later index, so the client failed the whole stream with
   * `missing output at index <n>`. The item carries the official reasoning shape
   * the buffered transport already publishes: one `summary_text` part whose text
   * is the provider's own `reasoning_content`.
   */
  const ensureReasoningItem = (): Record<string, unknown>[] => {
    if (state.reasoningOpen) return [];
    state.reasoningOpen = true;
    state.reasoningPending = false;
    state.reasoningIndex = state.nextOutputIndex++;
    return [
      {
        type: "response.output_item.added",
        output_index: state.reasoningIndex,
        item: { id: reasoningItemId(), type: "reasoning", status: "in_progress", summary: [] },
      },
      {
        type: "response.reasoning_summary_part.added",
        item_id: reasoningItemId(),
        output_index: state.reasoningIndex,
        summary_index: 0,
        part: { type: "summary_text", text: "" },
      },
    ];
  };

  /** One summary-text delta for the reasoning item that is currently open. */
  const reasoningDeltaEvent = (text: string): Record<string, unknown> => ({
    type: "response.reasoning_summary_text.delta",
    item_id: reasoningItemId(),
    output_index: state.reasoningIndex,
    summary_index: 0,
    delta: text,
  });

  /**
   * Accumulates one provider reasoning delta.
   *
   * The provider's normal order is reasoning before any answer item, and that
   * first item is streamed as it arrives. Once another output item has been
   * announced, a new reasoning segment cannot be announced without overlapping
   * that item for the single-active-item Codex consumer, so it is held and
   * delivered as one closed lifecycle at the terminal. No event ever targets a
   * reasoning item after it closes; a later segment becomes its own item.
   */
  const applyReasoningDelta = (text: string): Record<string, unknown>[] => {
    state.reasoning += text;
    if (state.reasoningOpen) {
      state.reasoningSegment += text;
      return [reasoningDeltaEvent(text)];
    }
    if (state.nextOutputIndex === 0) {
      const events = ensureReasoningItem();
      state.reasoningSegment += text;
      events.push(reasoningDeltaEvent(text));
      return events;
    }
    state.reasoningPending = true;
    state.reasoningSegment += text;
    return [];
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
  const terminalEnvelope = () => deepSeekTerminalEnvelope(responseId, requestedModel, createdAtSeconds, "completed", echo, state.finishReason, profile);

  const isCustomCall = (call: StreamToolCall): boolean => customToolNames.has(call.name);

  const announceToolCall = (call: StreamToolCall): Record<string, unknown>[] => {
    // Same single-active-item rule as the message: close the open reasoning item
    // before this tool call takes an index of its own.
    const events = closeReasoning();
    call.announced = true;
    call.outputIndex = state.nextOutputIndex++;
    const custom = isCustomCall(call);
    return [
      ...events,
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
    const events: Record<string, unknown>[] = [];
    if (typeof delta.reasoning_content === "string" && delta.reasoning_content) {
      events.push(...applyReasoningDelta(delta.reasoning_content));
    }
    if (typeof delta.content !== "string" || !delta.content) return events;
    state.text += delta.content;
    return [
      ...events,
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
      // A deferred reasoning segment must be flushed before the tool item it
      // precedes, or replaying this output attaches the reasoning to the wrong
      // (or no) assistant turn. The call keeps merging its fragmented name and
      // arguments here and is announced with them at the terminal.
      if (!call.announced && call.name && !state.reasoningPending) events.push(...announceToolCall(call));
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

  /**
   * Delivers a reasoning segment that could not be announced when it arrived
   * (it followed another output item). The item is opened, filled with one
   * summary-text delta, and closed in one batch after the answer items, so the
   * single-active-item consumer never sees it overlap another item and no event
   * targets an item that already completed.
   */
  const flushPendingReasoning = (): Record<string, unknown>[] => {
    if (state.reasoningOpen || !state.reasoningPending) return [];
    const events = ensureReasoningItem();
    events.push(reasoningDeltaEvent(state.reasoningSegment));
    return [...events, ...closeReasoning()];
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
      const usage = toResponsesUsage(chunk.usage, profile);
      if (usage) state.usage = usage;
      // An empty `choices` array is a valid frame, not a malformed one: on the
      // LithosAI wire the authoritative usage totals ride a trailing chunk with
      // no choices. The loop simply contributes no content events while the
      // usage above is still captured, and no `stream_options.include_usage`
      // request flag is required for this profile to receive it.
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
     * The terminal this stream will settle on, available before `finish` emits
     * it so telemetry records the same terminal the client receives. Derived
     * from the shared DeepSeek vocabulary mapping, so it cannot drift from the
     * event `finish` emits.
     */
    terminalKind: (): DeepSeekResponsesTerminalKind => deepSeekResponsesTerminalKind(state.finishReason, profile),
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
      // their names out of call order. The message closes first, then a deferred
      // reasoning segment is flushed, then the tool items close and any
      // reasoning item still open (it can only be the last announced item)
      // closes. Flushing before the tool items keeps the reasoning item ahead of
      // the tool call it belongs to in the delivered output, so history replay
      // attaches it to that assistant tool-call turn, and no two Codex-visible
      // items are ever open at once.
      const events = [...startEvents(), ...closeMessage(), ...flushPendingReasoning(), ...closeToolCalls(), ...closeReasoning()];
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
