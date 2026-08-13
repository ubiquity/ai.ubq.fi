import {
  RESPONSES_TERMINAL_EVENT_TYPES,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamIterator,
} from "./responses_stream.ts";
import { getString, isRecord } from "./utils.ts";

export type ResponsesSemanticKind = "text" | "reasoning" | "tool_call";

export const MAX_RESPONSES_PRECOMMIT_EVENTS = 10_000;
export const MAX_RESPONSES_PRECOMMIT_CHARS = 32 * 1024 * 1024;

const textTypes = new Set(["response.output_text.delta", "response.output_text.done"]);
const reasoningTypes = new Set([
  "response.reasoning_summary_text.delta",
  "response.reasoning_summary_text.done",
  "response.reasoning_text.delta",
  "response.reasoning_text.done",
]);
const executableToolTypes = new Set(["function_call", "custom_tool_call"]);

const nonEmptyText = (value: Record<string, unknown>): boolean =>
  [value.delta, value.text].some((item) => typeof item === "string" && item.length > 0);

const semanticKindFromOutput = (output: unknown): ResponsesSemanticKind | null => {
  if (!Array.isArray(output)) return null;
  for (const item of output) {
    if (!isRecord(item) || Array.isArray(item)) continue;
    if (item.type === "function_call") {
      if (
        typeof item.call_id === "string" && item.call_id.trim() && typeof item.name === "string" &&
        item.name.trim() && typeof item.arguments === "string"
      ) return "tool_call";
    }
    if (item.type === "custom_tool_call") {
      if (
        typeof item.call_id === "string" && item.call_id.trim() && typeof item.name === "string" &&
        item.name.trim() && typeof item.input === "string"
      ) return "tool_call";
    }
    if (item.type === "reasoning") {
      const values = [item.content, item.summary];
      if (
        values.some((value) =>
          Array.isArray(value) &&
          value.some((part) =>
            isRecord(part) && [part.text, part.delta].some((text) => typeof text === "string" && text.length > 0)
          )
        )
      ) return "reasoning";
    }
    if (!Array.isArray(item.content)) continue;
    if (
      item.content.some((part) =>
        isRecord(part) && (part.type === "output_text" || part.type === "text") &&
        typeof part.text === "string" && part.text.length > 0
      )
    ) return "text";
  }
  return null;
};

export const responsesEventSemanticKind = (event: ResponsesStreamEvent): ResponsesSemanticKind | null => {
  if (textTypes.has(event.type)) return nonEmptyText(event.value) ? "text" : null;
  if (reasoningTypes.has(event.type)) return nonEmptyText(event.value) ? "reasoning" : null;
  if (event.type === "response.output_item.done" && isRecord(event.value.item)) {
    const item = event.value.item;
    const itemType = getString(item.type) ?? "";
    if (executableToolTypes.has(itemType)) {
      const callId = getString(item.call_id)?.trim();
      const name = getString(item.name)?.trim();
      if (!callId || !name) return null;
      if (itemType === "function_call") return typeof item.arguments === "string" ? "tool_call" : null;
      return typeof item.input === "string" ? "tool_call" : null;
    }
    return semanticKindFromOutput([item]);
  }
  if (isRecord(event.value.response) && !Array.isArray(event.value.response)) {
    return semanticKindFromOutput(event.value.response.output);
  }
  return null;
};

export type PreparedResponsesStream = Readonly<{
  iterator: ResponsesStreamIterator;
  buffered: ResponsesStreamEvent[];
  bufferedChars: number;
  semantic: ResponsesStreamEvent | null;
  semanticKind: ResponsesSemanticKind | null;
  terminal: ResponsesStreamEvent | null;
}>;

export const appendResponsesPrecommitEvent = (
  buffered: ResponsesStreamEvent[],
  event: ResponsesStreamEvent,
  bufferedChars: number,
): number => {
  const nextChars = bufferedChars + event.raw.length;
  if (buffered.length >= MAX_RESPONSES_PRECOMMIT_EVENTS || nextChars > MAX_RESPONSES_PRECOMMIT_CHARS) {
    throw new ResponsesStreamError("Upstream Responses precommit buffer exceeded its limit.", {
      kind: "event_too_large",
    });
  }
  buffered.push(event);
  return nextChars;
};

/** Holds all provider events until semantic output or a valid terminal owns the attempt. */
export const prepareResponsesStreamForCommit = async (
  iterator: ResponsesStreamIterator,
): Promise<PreparedResponsesStream> => {
  const buffered: ResponsesStreamEvent[] = [];
  let bufferedChars = 0;
  try {
    while (true) {
      const next = await iterator.next();
      if (next.done || !next.value) {
        throw new ResponsesStreamError("Upstream Responses stream ended before semantic output.", {
          kind: "premature_eof",
        });
      }
      bufferedChars = appendResponsesPrecommitEvent(buffered, next.value, bufferedChars);
      const semanticKind = responsesEventSemanticKind(next.value);
      if (semanticKind) {
        return {
          iterator,
          buffered,
          bufferedChars,
          semantic: next.value,
          semanticKind,
          terminal: next.value.terminal ? next.value : null,
        };
      }
      if (next.value.terminal) {
        return { iterator, buffered, bufferedChars, semantic: null, semanticKind: null, terminal: next.value };
      }
    }
  } catch (error) {
    await iterator.return(error).catch(() => {});
    throw error;
  }
};

const sseRaw = (value: Record<string, unknown>): string =>
  `event: ${getString(value.type) ?? "message"}\ndata: ${JSON.stringify(value)}\n\n`;

export const responseEventFromValue = (value: Record<string, unknown>): ResponsesStreamEvent => {
  const type = getString(value.type)?.trim();
  if (!type) throw new ResponsesStreamError("Responses event rewrite omitted type.", { kind: "malformed_event" });
  return {
    raw: sseRaw(value),
    value,
    type,
    terminal: RESPONSES_TERMINAL_EVENT_TYPES.has(type),
  };
};

const incrementOutputIndex = (value: unknown): unknown =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value + 1 : value;

const withResponseOutputPrefix = (
  response: Record<string, unknown>,
  warningItem: Record<string, unknown>,
): Record<string, unknown> => ({
  ...response,
  output: [warningItem, ...(Array.isArray(response.output) ? response.output : [])],
});

export const rewriteResponsesEventForWarning = (
  event: ResponsesStreamEvent,
  warningItem: Record<string, unknown>,
  sequenceNumber: number,
): ResponsesStreamEvent => {
  const value: Record<string, unknown> = { ...event.value, sequence_number: sequenceNumber };
  if (Object.prototype.hasOwnProperty.call(value, "output_index")) {
    value.output_index = incrementOutputIndex(value.output_index);
  }
  if (
    (event.type === "response.completed" || event.type === "response.failed" ||
      event.type === "response.incomplete") &&
    isRecord(value.response) && !Array.isArray(value.response)
  ) {
    value.response = withResponseOutputPrefix(value.response, warningItem);
  }
  return responseEventFromValue(value);
};

export const rewriteResponsesEventSequence = (
  event: ResponsesStreamEvent,
  sequenceNumber: number,
): ResponsesStreamEvent => responseEventFromValue({ ...event.value, sequence_number: sequenceNumber });

export const buildFailoverWarningEvents = (
  actualModel: string,
  responseId: string,
  startingSequenceNumber = 0,
): Readonly<{ item: Record<string, unknown>; events: ResponsesStreamEvent[] }> => {
  const itemId = `msg_failover_${crypto.randomUUID().replace(/-/g, "")}`;
  const text =
    `⚠ Failover active: this response is from \`openrouter:${actualModel}\` because the Codex upstream was unavailable.`;
  const content = { type: "output_text", text, annotations: [] };
  const item: Record<string, unknown> = {
    id: itemId,
    type: "message",
    status: "completed",
    role: "assistant",
    content: [content],
  };
  const values: Record<string, unknown>[] = [
    {
      type: "response.output_item.added",
      response_id: responseId,
      output_index: 0,
      item: { ...item, status: "in_progress", content: [] },
    },
    {
      type: "response.content_part.added",
      response_id: responseId,
      item_id: itemId,
      output_index: 0,
      content_index: 0,
      part: { type: "output_text", text: "", annotations: [] },
    },
    {
      type: "response.output_text.delta",
      response_id: responseId,
      item_id: itemId,
      output_index: 0,
      content_index: 0,
      delta: text,
    },
    {
      type: "response.output_text.done",
      response_id: responseId,
      item_id: itemId,
      output_index: 0,
      content_index: 0,
      text,
    },
    {
      type: "response.content_part.done",
      response_id: responseId,
      item_id: itemId,
      output_index: 0,
      content_index: 0,
      part: content,
    },
    {
      type: "response.output_item.done",
      response_id: responseId,
      output_index: 0,
      item,
    },
  ].map((value, index) => ({ ...value, sequence_number: startingSequenceNumber + index }));
  return { item, events: values.map(responseEventFromValue) };
};

const idsFromEvent = (event: ResponsesStreamEvent): string[] => {
  const ids: string[] = [];
  const direct = getString(event.value.response_id)?.trim();
  if (direct) ids.push(direct);
  if (isRecord(event.value.response) && !Array.isArray(event.value.response)) {
    const nested = getString(event.value.response.id)?.trim();
    if (nested) ids.push(nested);
  }
  return ids;
};

export const responseIdFromEvents = (events: readonly ResponsesStreamEvent[]): string | null => {
  let responseId: string | null = null;
  for (const event of events) {
    for (const candidate of idsFromEvent(event)) {
      if (responseId && responseId !== candidate) {
        throw new ResponsesStreamError("Upstream Responses stream changed response identifiers.", {
          kind: "malformed_event",
        });
      }
      responseId = candidate;
    }
  }
  return responseId;
};

const syntheticFailureEvents = new WeakSet<ResponsesStreamEvent>();

export const failureEventAfterCommit = (
  responseId: string,
  sequenceNumber: number,
  output: readonly Record<string, unknown>[] = [],
  responseTemplate: Readonly<Record<string, unknown>> = {},
): ResponsesStreamEvent => {
  const event = responseEventFromValue({
    type: "response.failed",
    sequence_number: sequenceNumber,
    response: {
      ...responseTemplate,
      id: responseId,
      object: getString(responseTemplate.object) ?? "response",
      status: "failed",
      error: {
        type: "server_error",
        code: "upstream_stream_error",
        message: "The upstream stream ended unexpectedly.",
      },
      output: [...output],
    },
  });
  syntheticFailureEvents.add(event);
  return event;
};

const errorEventAfterCommit = (sequenceNumber: number): ResponsesStreamEvent => {
  const event = responseEventFromValue({
    type: "error",
    sequence_number: sequenceNumber,
    code: "upstream_stream_error",
    message: "The upstream stream ended unexpectedly.",
    param: null,
  });
  syntheticFailureEvents.add(event);
  return event;
};

export const isSyntheticResponsesFailureEvent = (event: ResponsesStreamEvent): boolean =>
  syntheticFailureEvents.has(event);

type OwnedResponsesStreamOptions = Readonly<{
  initial: readonly ResponsesStreamEvent[];
  iterator: ResponsesStreamIterator;
  responseId: string | null;
  warning?: Readonly<{ model: string }>;
  signal?: AbortSignal;
  downstreamSignal?: AbortSignal;
  onEvent?: (event: ResponsesStreamEvent) => void | Promise<void>;
  validateEvent?: (event: ResponsesStreamEvent) => void;
  onFailure?: (error: unknown) => void | Promise<void>;
  onCancel?: (reason: unknown) => void | Promise<void>;
}>;

const invoke = (callback: (() => void | Promise<void>) | undefined): void => {
  if (!callback) return;
  try {
    void Promise.resolve(callback()).catch(() => {});
  } catch {
    // Observability callbacks never own stream delivery.
  }
};

/** Owns event ordering and guarantees at most one client-visible terminal event. */
export const createOwnedResponsesStream = (
  options: OwnedResponsesStreamOptions,
): ReadableStream<Uint8Array> => {
  const encoder = new TextEncoder();
  const localAbort = new AbortController();
  const initial = [...options.initial];
  let warningItem: Record<string, unknown> | null = null;
  let responseId = options.responseId;
  let sequenceNumber = options.warning
    ? 0
    : initial.reduce((max, event) =>
      typeof event.value.sequence_number === "number" && Number.isSafeInteger(event.value.sequence_number)
        ? Math.max(max, event.value.sequence_number + 1)
        : max, 0);
  let closed = false;
  let terminalEmitted = false;
  let responseCreatedObserved = false;
  const queue: ResponsesStreamEvent[] = [];
  let responseTemplate: Record<string, unknown> = {};
  const completedOutputItems: Record<string, unknown>[] = [];
  const completedOutputItemIds = new Map<string, number>();
  const accumulatedText = new Map<string, { id: string; text: string; completed: boolean }>();

  if (options.warning) {
    if (!responseId) {
      throw new ResponsesStreamError("OpenRouter stream omitted a response identifier.", { kind: "malformed_event" });
    }
    const createdIndex = initial.findIndex((event) => event.type === "response.created");
    if (createdIndex < 0) {
      throw new ResponsesStreamError("OpenRouter stream omitted response.created.", { kind: "malformed_event" });
    }
    const created = initial.splice(createdIndex, 1)[0]!;
    options.validateEvent?.(created);
    queue.push(rewriteResponsesEventSequence(created, sequenceNumber++));
    const leadingSetup: ResponsesStreamEvent[] = [];
    while (
      initial.length &&
      (initial[0]!.type === "response.in_progress" || initial[0]!.type === "response.queued")
    ) leadingSetup.push(initial.shift()!);
    for (const event of leadingSetup) options.validateEvent?.(event);
    queue.push(...leadingSetup.map((event) => rewriteResponsesEventSequence(event, sequenceNumber++)));
    const warning = buildFailoverWarningEvents(options.warning.model, responseId, sequenceNumber);
    warningItem = warning.item;
    queue.push(...warning.events);
    sequenceNumber += warning.events.length;
    for (const event of initial) options.validateEvent?.(event);
    queue.push(...initial.map((event) => rewriteResponsesEventForWarning(event, warning.item, sequenceNumber++)));
  } else {
    for (const event of initial) options.validateEvent?.(event);
    queue.push(...initial);
  }

  const warningItemId = (): string | null => warningItem ? getString(warningItem.id)?.trim() ?? null : null;
  const eventItemId = (event: ResponsesStreamEvent): string | null => {
    const direct = getString(event.value.item_id)?.trim();
    if (direct) return direct;
    if (isRecord(event.value.item) && !Array.isArray(event.value.item)) {
      return getString(event.value.item.id)?.trim() ?? null;
    }
    return null;
  };
  const isWarningEvent = (event: ResponsesStreamEvent): boolean => {
    const warningId = warningItemId();
    return warningId !== null && eventItemId(event) === warningId;
  };
  const itemHasOutputText = (item: Record<string, unknown>): boolean =>
    Array.isArray(item.content) && item.content.some((part) =>
      isRecord(part) && !Array.isArray(part) &&
      (part.type === "output_text" || part.type === "text") &&
      typeof part.text === "string" && part.text.length > 0
    );
  const rememberText = (event: ResponsesStreamEvent): void => {
    if (isWarningEvent(event)) return;
    const text = event.type === "response.output_text.delta"
      ? getString(event.value.delta)
      : event.type === "response.output_text.done"
      ? getString(event.value.text)
      : null;
    if (!text) return;
    const id = eventItemId(event) ?? `msg_recovered_${getString(event.value.output_index) ?? accumulatedText.size}`;
    const current = accumulatedText.get(id);
    if (event.type === "response.output_text.done") {
      if (!current || text.startsWith(current.text)) accumulatedText.set(id, { id, text, completed: true });
      return;
    }
    accumulatedText.set(id, { id, text: `${current?.text ?? ""}${text}`, completed: current?.completed ?? false });
  };
  const observeVisibleEvent = (event: ResponsesStreamEvent): void => {
    if (event.type === "response.created") responseCreatedObserved = true;
    const valueResponse = event.value.response;
    if (isRecord(valueResponse) && !Array.isArray(valueResponse)) {
      responseTemplate = { ...responseTemplate, ...valueResponse };
    }
    rememberText(event);
    if (event.type !== "response.output_item.done" || isWarningEvent(event)) return;
    if (!isRecord(event.value.item) || Array.isArray(event.value.item)) return;
    const item = { ...event.value.item };
    const id = getString(item.id)?.trim();
    if (id && completedOutputItemIds.has(id)) {
      completedOutputItems[completedOutputItemIds.get(id)!] = item;
      return;
    }
    if (id) completedOutputItemIds.set(id, completedOutputItems.length);
    completedOutputItems.push(item);
  };
  const failureOutput = (): Record<string, unknown>[] => {
    const output = warningItem ? [{ ...warningItem }] : [];
    const outputById = new Map<string, number>();
    const warningId = warningItemId();
    if (warningId) outputById.set(warningId, 0);
    for (const item of completedOutputItems) {
      const id = getString(item.id)?.trim();
      if (id && outputById.has(id)) continue;
      if (id) outputById.set(id, output.length);
      output.push({ ...item });
    }
    for (const recovered of accumulatedText.values()) {
      const existingIndex = outputById.get(recovered.id);
      if (existingIndex !== undefined && itemHasOutputText(output[existingIndex]!)) continue;
      const message = {
        id: recovered.id,
        type: "message",
        status: recovered.completed ? "completed" : "incomplete",
        role: "assistant",
        content: [{ type: "output_text", text: recovered.text, annotations: [] }],
      };
      if (existingIndex === undefined) {
        outputById.set(recovered.id, output.length);
        output.push(message);
      } else {
        output[existingIndex] = { ...output[existingIndex], ...message };
      }
    }
    return output;
  };
  const syntheticFailure = (): ResponsesStreamEvent => {
    return responseCreatedObserved
      ? failureEventAfterCommit(
        responseId ?? `resp_${crypto.randomUUID().replace(/-/g, "")}`,
        sequenceNumber++,
        failureOutput(),
        responseTemplate,
      )
      : errorEventAfterCommit(sequenceNumber++);
  };
  const advanceSequence = (event: ResponsesStreamEvent): void => {
    const value = event.value.sequence_number;
    if (typeof value === "number" && Number.isSafeInteger(value) && value >= sequenceNumber) {
      sequenceNumber = value + 1;
    }
  };

  const nextVisible = async (): Promise<ResponsesStreamEvent | null> => {
    if (queue.length) return queue.shift()!;
    const next = await options.iterator.next();
    if (next.done || !next.value) return null;
    options.validateEvent?.(next.value);
    const candidate = responseIdFromEvents([next.value]);
    if (candidate && responseId && candidate !== responseId) {
      throw new ResponsesStreamError("Upstream Responses stream changed response identifiers.", {
        kind: "malformed_event",
      });
    }
    responseId ??= candidate;
    if (next.value.type === "error") {
      throw new ResponsesStreamError("Upstream Responses stream emitted an error event after commitment.", {
        kind: "malformed_event",
      });
    }
    if (!warningItem) return next.value;
    return rewriteResponsesEventForWarning(next.value, warningItem, sequenceNumber++);
  };

  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        const event = await nextVisible();
        if (closed) return;
        if (!event) {
          const failure = syntheticFailure();
          terminalEmitted = true;
          closed = true;
          controller.enqueue(encoder.encode(failure.raw));
          controller.close();
          invoke(() => options.onEvent?.(failure));
          invoke(() =>
            options.onFailure?.(
              new ResponsesStreamError("Responses stream ended without a terminal.", {
                kind: "premature_eof",
              }),
            )
          );
          return;
        }
        if (terminalEmitted) return;
        observeVisibleEvent(event);
        advanceSequence(event);
        terminalEmitted = event.terminal;
        controller.enqueue(encoder.encode(event.raw));
        invoke(() => options.onEvent?.(event));
        if (event.terminal) {
          closed = true;
          controller.close();
          await options.iterator.return("Responses terminal event forwarded").catch(() => {});
        }
      } catch (error) {
        if (closed) return;
        if (options.downstreamSignal?.aborted || options.signal?.aborted || localAbort.signal.aborted) {
          closed = true;
          controller.close();
          invoke(() => options.onFailure?.(error));
          return;
        }
        const failure = syntheticFailure();
        terminalEmitted = true;
        closed = true;
        controller.enqueue(encoder.encode(failure.raw));
        controller.close();
        invoke(() => options.onEvent?.(failure));
        invoke(() => options.onFailure?.(error));
      }
    },
    cancel(reason) {
      if (closed) return;
      closed = true;
      localAbort.abort(reason);
      invoke(() => options.onCancel?.(reason));
      void options.iterator.return(reason).catch(() => {});
    },
  });
};
