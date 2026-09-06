import {
  RESPONSES_TERMINAL_EVENT_TYPES,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamFailureKind,
  type ResponsesStreamIterator,
} from "./responses_stream.ts";
import { getString, isRecord } from "./utils.ts";

export type ResponsesSemanticKind = "text" | "tool_call";

export const MAX_RESPONSES_PRECOMMIT_EVENTS = 10_000;
export const MAX_RESPONSES_PRECOMMIT_CHARS = 32 * 1024 * 1024;

const textTypes = new Set(["response.output_text.delta", "response.output_text.done"]);
const refusalTypes = new Set(["response.refusal.delta", "response.refusal.done"]);
const reasoningTextProgressFields: ReadonlyMap<string, "delta" | "text"> = new Map([
  ["response.reasoning_summary_text.delta", "delta"],
  ["response.reasoning_summary_text.done", "text"],
  ["response.reasoning_text.delta", "delta"],
  ["response.reasoning_text.done", "text"],
]);
const reasoningSummaryPartProgressTypes = new Set([
  "response.reasoning_summary_part.added",
  "response.reasoning_summary_part.done",
]);
const executableToolTypes = new Set(["function_call", "custom_tool_call"]);
const hostedToolTypes = new Set([
  "code_interpreter_call",
  "computer_call",
  "file_search_call",
  "image_generation_call",
  "mcp_call",
  "web_search_call",
]);
const hostedToolCompletedEventTypes = new Set([...hostedToolTypes].map((type) => `response.${type}.completed`));
const hostedToolTerminalEventTypes = new Set([
  ...hostedToolCompletedEventTypes,
  ...[...hostedToolTypes].map((type) => `response.${type}.in_progress`),
  ...[...hostedToolTypes].map((type) => `response.${type}.failed`),
]);
const imagePartialEventType = "response.image_generation_call.partial_image";

const nonEmptyText = (value: Record<string, unknown>): boolean =>
  [value.delta, value.text].some((item) => typeof item === "string" && item.length > 0);

const semanticKindFromOutput = (
  output: unknown,
  ignoredOutputItemId: string | null = null,
): ResponsesSemanticKind | null => {
  if (!Array.isArray(output)) return null;
  for (const item of output) {
    if (!isRecord(item) || Array.isArray(item)) continue;
    if (ignoredOutputItemId !== null && getString(item.id)?.trim() === ignoredOutputItemId) continue;
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
    if (
      typeof item.type === "string" && hostedToolTypes.has(item.type) &&
      (item.status === "in_progress" || item.status === "completed" || item.status === "failed")
    ) {
      return "tool_call";
    }
    if (item.type === "reasoning") continue;
    if (!Array.isArray(item.content)) continue;
    if (
      item.content.some((part) =>
        isRecord(part) && (
          ((part.type === "output_text" || part.type === "text") &&
            typeof part.text === "string" && part.text.length > 0) ||
          (part.type === "refusal" && typeof part.refusal === "string" && part.refusal.length > 0)
        )
      )
    ) return "text";
  }
  return null;
};

const responsesEventSemanticKindWithIgnoredOutputItem = (
  event: ResponsesStreamEvent,
  ignoredOutputItemId: string | null,
): ResponsesSemanticKind | null => {
  if (textTypes.has(event.type)) return nonEmptyText(event.value) ? "text" : null;
  if (refusalTypes.has(event.type)) {
    return (nonEmptyText(event.value) || (typeof event.value.refusal === "string" && event.value.refusal.length > 0))
      ? "text"
      : null;
  }
  if (event.type === "response.content_part.done" && isRecord(event.value.part)) {
    const part = event.value.part;
    if (
      (part.type === "output_text" || part.type === "text") && typeof part.text === "string" && part.text.length > 0
    ) return "text";
    if (part.type === "refusal" && typeof part.refusal === "string" && part.refusal.length > 0) return "text";
  }
  if (hostedToolTerminalEventTypes.has(event.type)) return "tool_call";
  if (event.type === imagePartialEventType) {
    return [event.value.partial_image_b64, event.value.partial_image, event.value.result]
        .some((value) => typeof value === "string" && value.length > 0)
      ? "tool_call"
      : null;
  }
  if (
    (event.type === "response.output_item.added" || event.type === "response.output_item.done") &&
    isRecord(event.value.item)
  ) {
    const item = event.value.item;
    const itemType = getString(item.type) ?? "";
    if (event.type === "response.output_item.done" && executableToolTypes.has(itemType)) {
      const callId = getString(item.call_id)?.trim();
      const name = getString(item.name)?.trim();
      if (!callId || !name) return null;
      if (itemType === "function_call") return typeof item.arguments === "string" ? "tool_call" : null;
      return typeof item.input === "string" ? "tool_call" : null;
    }
    if (hostedToolTypes.has(itemType)) return semanticKindFromOutput([item], ignoredOutputItemId);
    if (event.type === "response.output_item.added") return null;
    return semanticKindFromOutput([item], ignoredOutputItemId);
  }
  if (isRecord(event.value.response) && !Array.isArray(event.value.response)) {
    return semanticKindFromOutput(event.value.response.output, ignoredOutputItemId);
  }
  const topLevelOutputKind = semanticKindFromOutput(event.value.output, ignoredOutputItemId);
  if (topLevelOutputKind) return topLevelOutputKind;
  return null;
};

export const responsesEventSemanticKind = (event: ResponsesStreamEvent): ResponsesSemanticKind | null =>
  responsesEventSemanticKindWithIgnoredOutputItem(event, null);

/** Reports active hidden-reasoning work without classifying it as client-visible output. */
export const responsesEventReportsProgress = (event: ResponsesStreamEvent): boolean => {
  const textField = reasoningTextProgressFields.get(event.type);
  if (textField) {
    const value = event.value[textField];
    const index = event.type.startsWith("response.reasoning_summary_")
      ? event.value.summary_index
      : event.value.content_index;
    return typeof value === "string" && value.length > 0 && typeof index === "number" &&
      Number.isSafeInteger(index) && index >= 0;
  }
  if (reasoningSummaryPartProgressTypes.has(event.type)) {
    const summaryIndex = event.value.summary_index;
    return typeof summaryIndex === "number" && Number.isSafeInteger(summaryIndex) && summaryIndex >= 0;
  }
  if (
    (event.type === "response.output_item.added" || event.type === "response.output_item.done") &&
    isRecord(event.value.item) && !Array.isArray(event.value.item) && event.value.item.type === "reasoning"
  ) {
    const itemId = getString(event.value.item.id)?.trim();
    return Boolean(itemId) &&
      (Array.isArray(event.value.item.summary) || Array.isArray(event.value.item.content));
  }
  return false;
};

const reasoningLifecycleProgressKey = (event: ResponsesStreamEvent): string | null => {
  if (reasoningSummaryPartProgressTypes.has(event.type)) {
    return `${event.type}:${getString(event.value.item_id) ?? ""}:${String(event.value.summary_index)}`;
  }
  if (
    (event.type === "response.output_item.added" || event.type === "response.output_item.done") &&
    isRecord(event.value.item) && !Array.isArray(event.value.item) && event.value.item.type === "reasoning"
  ) {
    const itemId = getString(event.value.item.id)?.trim();
    return itemId ? `${event.type}:${itemId}` : null;
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
  options: Readonly<{
    onEvent?: (event: ResponsesStreamEvent) => void;
    onProgress?: (event: ResponsesStreamEvent) => void;
    releaseOnProgress?: boolean;
  }> = {},
): Promise<PreparedResponsesStream> => {
  const buffered: ResponsesStreamEvent[] = [];
  const reportedLifecycleProgress = new Set<string>();
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
      options.onEvent?.(next.value);
      if (responsesEventReportsProgress(next.value)) {
        const lifecycleKey = reasoningLifecycleProgressKey(next.value);
        if (lifecycleKey === null || !reportedLifecycleProgress.has(lifecycleKey)) {
          if (lifecycleKey !== null) reportedLifecycleProgress.add(lifecycleKey);
          options.onProgress?.(next.value);
          if (options.releaseOnProgress) {
            return {
              iterator,
              buffered,
              bufferedChars,
              semantic: null,
              semanticKind: null,
              terminal: null,
            };
          }
        }
      }
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
    `⚠ Failover active: this response is from \`removed_provider:${actualModel}\` because the Codex upstream was unavailable.`;
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

export type OwnedResponsesStreamFailureDetails = Readonly<{
  failureKind: ResponsesStreamFailureKind;
  responseCreatedObserved: boolean;
  semanticCommitmentObserved: boolean;
  syntheticTerminalType: "response.failed" | "error" | null;
  upstreamTerminal: ResponsesStreamEvent | null;
}>;

export const failureEventAfterCommit = (
  responseId: string,
  sequenceNumber: number,
  output: readonly Record<string, unknown>[] = [],
  responseTemplate: Readonly<Record<string, unknown>> = {},
): ResponsesStreamEvent => {
  const responseWithoutUsage = { ...responseTemplate };
  delete responseWithoutUsage.usage;
  const event = responseEventFromValue({
    type: "response.failed",
    sequence_number: sequenceNumber,
    response: {
      // Usage is authoritative only on an upstream terminal. A non-terminal
      // response snapshot may contain null, partial, or provider-specific usage;
      // carrying it into the synthetic failure would turn missing usage into an
      // invalid or fabricated terminal observation.
      ...responseWithoutUsage,
      id: responseId,
      object: getString(responseTemplate.object) ?? "response",
      status: "failed",
      error: {
        code: "server_error",
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
    code: "server_error",
    message: "The upstream stream ended unexpectedly.",
    param: null,
  });
  syntheticFailureEvents.add(event);
  return event;
};

const EMPTY_UPSTREAM_COMPLETION_MESSAGE = "Upstream response completed with no translated semantic output.";

const emptyCompletionEventAfterCommit = (sequenceNumber: number): ResponsesStreamEvent => {
  const event = responseEventFromValue({
    type: "error",
    sequence_number: sequenceNumber,
    code: "empty_upstream_completion",
    message: EMPTY_UPSTREAM_COMPLETION_MESSAGE,
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
  abortUpstream?: (reason?: unknown) => void;
  onEvent?: (event: ResponsesStreamEvent) => void | Promise<void>;
  validateEvent?: (event: ResponsesStreamEvent) => void;
  onFailure?: (error: unknown, details: OwnedResponsesStreamFailureDetails) => void | Promise<void>;
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
  let semanticCommitmentObserved = false;
  const queue: ResponsesStreamEvent[] = [];
  const originalUpstreamEvents = new WeakMap<ResponsesStreamEvent, ResponsesStreamEvent>();
  let responseTemplate: Record<string, unknown> = {};
  const completedOutputItems: Record<string, unknown>[] = [];
  const completedOutputItemIds = new Map<string, number>();
  const accumulatedText = new Map<string, { id: string; text: string; completed: boolean; refusal: boolean }>();

  if (options.warning) {
    if (!responseId) {
      throw new ResponsesStreamError("RemovedProvider stream omitted a response identifier.", {
        kind: "malformed_event",
      });
    }
    const createdIndex = initial.findIndex((event) => event.type === "response.created");
    if (createdIndex < 0) {
      throw new ResponsesStreamError("RemovedProvider stream omitted response.created.", { kind: "malformed_event" });
    }
    const created = initial.splice(createdIndex, 1)[0]!;
    options.validateEvent?.(created);
    const rewrittenCreated = rewriteResponsesEventSequence(created, sequenceNumber++);
    originalUpstreamEvents.set(rewrittenCreated, created);
    queue.push(rewrittenCreated);
    const leadingSetup: ResponsesStreamEvent[] = [];
    while (
      initial.length &&
      (initial[0]!.type === "response.in_progress" || initial[0]!.type === "response.queued")
    ) leadingSetup.push(initial.shift()!);
    for (const event of leadingSetup) options.validateEvent?.(event);
    for (const event of leadingSetup) {
      const rewritten = rewriteResponsesEventSequence(event, sequenceNumber++);
      originalUpstreamEvents.set(rewritten, event);
      queue.push(rewritten);
    }
    const warning = buildFailoverWarningEvents(options.warning.model, responseId, sequenceNumber);
    warningItem = warning.item;
    queue.push(...warning.events);
    sequenceNumber += warning.events.length;
    for (const event of initial) options.validateEvent?.(event);
    for (const event of initial) {
      const rewritten = rewriteResponsesEventForWarning(event, warning.item, sequenceNumber++);
      originalUpstreamEvents.set(rewritten, event);
      queue.push(rewritten);
    }
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
    const refusal = refusalTypes.has(event.type);
    const text = event.type === "response.output_text.delta" || event.type === "response.refusal.delta"
      ? getString(event.value.delta)
      : event.type === "response.output_text.done"
      ? getString(event.value.text)
      : event.type === "response.refusal.done"
      ? getString(event.value.refusal)
      : null;
    if (!text) return;
    const id = eventItemId(event) ?? `msg_recovered_${getString(event.value.output_index) ?? accumulatedText.size}`;
    const current = accumulatedText.get(id);
    if (event.type === "response.output_text.done" || event.type === "response.refusal.done") {
      if (!current || text.startsWith(current.text)) accumulatedText.set(id, { id, text, completed: true, refusal });
      return;
    }
    accumulatedText.set(id, {
      id,
      text: `${current?.text ?? ""}${text}`,
      completed: current?.completed ?? false,
      refusal,
    });
  };
  const observeVisibleEvent = (event: ResponsesStreamEvent): void => {
    if (event.type === "response.created") responseCreatedObserved = true;
    if (
      !isWarningEvent(event) &&
      responsesEventSemanticKindWithIgnoredOutputItem(event, warningItemId())
    ) semanticCommitmentObserved = true;
    const candidateResponseId = responseIdFromEvents([event]);
    if (candidateResponseId && responseId && candidateResponseId !== responseId) {
      throw new ResponsesStreamError("Upstream Responses stream changed response identifiers.", {
        kind: "malformed_event",
      });
    }
    responseId ??= candidateResponseId;
    const valueResponse = event.value.response;
    if (isRecord(valueResponse) && !Array.isArray(valueResponse)) {
      responseTemplate = { ...responseTemplate, ...valueResponse };
    }
    rememberText(event);
    if (isWarningEvent(event)) return;
    let item: Record<string, unknown> | null = null;
    const compatibilityOutput = event.type === "response.output"
      ? event.value.output ?? (isRecord(valueResponse) ? valueResponse.output : undefined)
      : undefined;
    if (Array.isArray(compatibilityOutput)) {
      for (const outputItem of compatibilityOutput) {
        if (!isRecord(outputItem)) continue;
        const id = getString(outputItem.id)?.trim();
        if (id && completedOutputItemIds.has(id)) {
          completedOutputItems[completedOutputItemIds.get(id)!] = { ...outputItem };
        } else {
          if (id) completedOutputItemIds.set(id, completedOutputItems.length);
          completedOutputItems.push({ ...outputItem });
        }
      }
    }
    if (
      (event.type === "response.output_item.added" || event.type === "response.output_item.done") &&
      isRecord(event.value.item) && !Array.isArray(event.value.item)
    ) {
      item = { ...event.value.item };
      if (event.type === "response.output_item.added") item.status = "incomplete";
    } else if (hostedToolTerminalEventTypes.has(event.type)) {
      const lifecycle = event.type.split(".").at(-1)!;
      const type = event.type.slice("response.".length, -(lifecycle.length + 1));
      const existingId = eventItemId(event);
      const existing = existingId ? completedOutputItems[completedOutputItemIds.get(existingId) ?? -1] : undefined;
      item = {
        ...(existing ?? {}),
        id: eventItemId(event) ?? `tool_recovered_${completedOutputItems.length}`,
        type,
        status: lifecycle === "completed" ? "completed" : lifecycle === "failed" ? "failed" : "in_progress",
      };
    }
    if (!item) return;
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
        content: recovered.refusal
          ? [{ type: "refusal", refusal: recovered.text }]
          : [{ type: "output_text", text: recovered.text, annotations: [] }],
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
    return semanticCommitmentObserved
      ? failureEventAfterCommit(
        responseId ?? `resp_${crypto.randomUUID().replace(/-/g, "")}`,
        sequenceNumber++,
        failureOutput(),
        responseTemplate,
      )
      : errorEventAfterCommit(sequenceNumber++);
  };
  const failureKindFor = (error: unknown): ResponsesStreamFailureKind =>
    error instanceof ResponsesStreamError ? error.kind : "read_error";
  const failureDetails = (
    error: unknown,
    syntheticTerminalType: OwnedResponsesStreamFailureDetails["syntheticTerminalType"],
    upstreamTerminal: ResponsesStreamEvent | null = null,
  ): OwnedResponsesStreamFailureDetails => ({
    failureKind: failureKindFor(error),
    responseCreatedObserved,
    semanticCommitmentObserved,
    syntheticTerminalType,
    upstreamTerminal,
  });
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
    if (!warningItem) return next.value;
    const rewritten = rewriteResponsesEventForWarning(next.value, warningItem, sequenceNumber++);
    originalUpstreamEvents.set(rewritten, next.value);
    return rewritten;
  };

  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        const event = await nextVisible();
        if (closed) return;
        if (!event) {
          const failure = syntheticFailure();
          const details = failureDetails(
            new ResponsesStreamError("Responses stream ended without a terminal.", { kind: "premature_eof" }),
            failure.type === "response.failed" || failure.type === "error" ? failure.type : null,
          );
          terminalEmitted = true;
          closed = true;
          controller.enqueue(encoder.encode(failure.raw));
          controller.close();
          invoke(() => options.onEvent?.(failure));
          invoke(() =>
            options.onFailure?.(
              new ResponsesStreamError("Responses stream ended without a terminal.", { kind: "premature_eof" }),
              details,
            )
          );
          return;
        }
        if (terminalEmitted) return;
        observeVisibleEvent(event);
        advanceSequence(event);
        if (event.type === "response.completed" && !semanticCommitmentObserved) {
          const error = new ResponsesStreamError(EMPTY_UPSTREAM_COMPLETION_MESSAGE, {
            kind: "empty_upstream_completion",
          });
          const failure = emptyCompletionEventAfterCommit(sequenceNumber++);
          const details = failureDetails(error, "error", originalUpstreamEvents.get(event) ?? event);
          terminalEmitted = true;
          closed = true;
          controller.enqueue(encoder.encode(failure.raw));
          controller.close();
          await options.iterator.return(error).catch(() => {});
          invoke(() => options.onEvent?.(failure));
          invoke(() => options.onFailure?.(error, details));
          return;
        }
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
          await options.iterator.return(error).catch(() => {});
          invoke(() => options.onFailure?.(error, failureDetails(error, null)));
          return;
        }
        const failure = syntheticFailure();
        const details = failureDetails(
          error,
          failure.type === "response.failed" || failure.type === "error" ? failure.type : null,
        );
        terminalEmitted = true;
        closed = true;
        controller.enqueue(encoder.encode(failure.raw));
        controller.close();
        await options.iterator.return(error).catch(() => {});
        invoke(() => options.onEvent?.(failure));
        invoke(() => options.onFailure?.(error, details));
      }
    },
    cancel(reason) {
      if (closed) return;
      closed = true;
      localAbort.abort(reason);
      options.abortUpstream?.(reason);
      invoke(() => options.onCancel?.(reason));
      void options.iterator.return(reason).catch(() => {});
    },
  });
};
