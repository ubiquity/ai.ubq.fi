import { getString, isRecord } from "./utils.ts";
import { STREAM_FIRST_EVENT_DEADLINE_MS, STREAM_INACTIVITY_DEADLINE_MS } from "./inference_deadline.ts";

export const RESPONSES_TERMINAL_EVENT_TYPES = new Set([
  "error",
  "response.completed",
  "response.failed",
  "response.incomplete",
]);

export type ResponsesStreamEvent = Readonly<{
  raw: string;
  value: Record<string, unknown>;
  type: string;
  terminal: boolean;
}>;

export type ResponsesStreamFailureKind =
  | "malformed_event"
  | "premature_eof"
  | "read_error"
  | "inactivity_timeout"
  | "event_too_large";

export const MAX_RESPONSES_SSE_EVENT_BYTES = 16 * 1024 * 1024;

export type ResponsesStreamIterator = AsyncGenerator<ResponsesStreamEvent, unknown, unknown>;

export type PreflightedResponsesStream = Readonly<{
  first: ResponsesStreamEvent;
  iterator: ResponsesStreamIterator;
}>;

export class ResponsesStreamError extends Error {
  readonly kind: ResponsesStreamFailureKind;

  constructor(message: string, options?: ErrorOptions & { kind?: ResponsesStreamFailureKind }) {
    super(message, options);
    this.name = "ResponsesStreamError";
    this.kind = options?.kind ?? "read_error";
  }
}

const parseEventBlock = (raw: string): ResponsesStreamEvent | null => {
  const data: string[] = [];
  for (const line of raw.split(/\r\n|\r|\n/)) {
    if (!line || line.startsWith(":")) continue;
    if (line === "data") data.push("");
    else if (line.startsWith("data:")) data.push(line.slice(5).replace(/^ /, ""));
  }
  if (!data.length) return null;

  let value: unknown;
  try {
    value = JSON.parse(data.join("\n"));
  } catch (cause) {
    throw new ResponsesStreamError("Upstream emitted malformed Responses SSE JSON.", {
      cause,
      kind: "malformed_event",
    });
  }
  if (!isRecord(value)) {
    throw new ResponsesStreamError("Upstream emitted a non-object Responses SSE event.", {
      kind: "malformed_event",
    });
  }
  const type = getString(value.type)?.trim();
  if (!type) {
    throw new ResponsesStreamError("Upstream emitted a Responses SSE event without a type.", {
      kind: "malformed_event",
    });
  }
  if (
    (type === "error" && !isRecord(value.error)) ||
    (type !== "error" && RESPONSES_TERMINAL_EVENT_TYPES.has(type) && !isRecord(value.response))
  ) {
    throw new ResponsesStreamError("Upstream emitted a Responses terminal event with an invalid payload.", {
      kind: "malformed_event",
    });
  }
  return { raw, value, type, terminal: RESPONSES_TERMINAL_EVENT_TYPES.has(type) };
};

export const readResponsesStream = async function* (
  stream: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
  options: Readonly<{ firstEventTimeoutMs?: number; inactivityTimeoutMs?: number }> = {},
): ResponsesStreamIterator {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  // The event buffer grows geometrically and never exceeds the protocol's
  // ceiling. Unlike a string accumulator, this neither re-encodes every
  // fragmented event nor allocates an unbounded intermediate string.
  let eventBuffer = new Uint8Array(Math.min(4_096, MAX_RESPONSES_SSE_EVENT_BYTES));
  let eventLength = 0;
  let thirdPreviousByte = -1;
  let secondPreviousByte = -1;
  let previousByte = -1;
  let terminal = false;
  let readerDone = false;
  let cancelStarted = false;
  const cancelReaderOnce = (reason: unknown): void => {
    if (cancelStarted || readerDone) return;
    cancelStarted = true;
    void reader.cancel(reason).catch(() => {});
  };
  const abort = () => cancelReaderOnce(signal?.reason);
  signal?.addEventListener("abort", abort, { once: true });
  let sawEvent = false;
  const firstEventDeadlineAtMs = Date.now() + (options.firstEventTimeoutMs ?? STREAM_FIRST_EVENT_DEADLINE_MS);
  const oversizedEvent = (): ResponsesStreamError =>
    new ResponsesStreamError("Upstream emitted an oversized Responses SSE event.", {
      kind: "event_too_large",
    });
  const appendEventBytes = (value: Uint8Array): void => {
    const nextLength = eventLength + value.byteLength;
    if (nextLength > MAX_RESPONSES_SSE_EVENT_BYTES) throw oversizedEvent();
    if (nextLength > eventBuffer.byteLength) {
      const nextCapacity = Math.min(
        MAX_RESPONSES_SSE_EVENT_BYTES,
        Math.max(nextLength, eventBuffer.byteLength * 2),
      );
      const next = new Uint8Array(nextCapacity);
      next.set(eventBuffer.subarray(0, eventLength));
      eventBuffer = next;
    }
    eventBuffer.set(value, eventLength);
    eventLength = nextLength;
  };
  const takeEvent = (): string => {
    const raw = decoder.decode(eventBuffer.subarray(0, eventLength));
    eventLength = 0;
    thirdPreviousByte = -1;
    secondPreviousByte = -1;
    previousByte = -1;
    return raw;
  };
  const isEventBoundary = (byte: number): boolean => {
    // CRLF is one line terminator. The CR in CRLFCRLF must not terminate
    // early, while mixed LF+CRLF framing (\n\r\n) must still split.
    if (byte === 10) {
      return previousByte === 10 ||
        (previousByte === 13 && thirdPreviousByte === 13 && secondPreviousByte === 10);
    }
    if (byte !== 13) return false;
    if (previousByte === 13) return true;
    return previousByte === 10 && secondPreviousByte !== 13;
  };
  const advanceBoundaryState = (byte: number): void => {
    thirdPreviousByte = secondPreviousByte;
    secondPreviousByte = previousByte;
    previousByte = byte;
  };
  const markParsedEvent = (parsed: ResponsesStreamEvent): void => {
    sawEvent = true;
    if (parsed.terminal) {
      terminal = true;
      cancelReaderOnce("Responses terminal event received");
    }
  };
  const processTrailingEvent = (): ResponsesStreamEvent | null => {
    if (!eventLength) return null;
    const parsed = parseEventBlock(takeEvent());
    if (parsed) markParsedEvent(parsed);
    return parsed;
  };
  const ensurePendingSegmentWithinLimit = (pendingBytes: number): void => {
    if (eventLength + pendingBytes > MAX_RESPONSES_SSE_EVENT_BYTES) {
      throw new ResponsesStreamError("Upstream emitted an oversized Responses SSE event.", {
        kind: "event_too_large",
      });
    }
  };
  const readWithDeadline = async (): Promise<ReadableStreamReadResult<Uint8Array>> => {
    const timeoutMs = sawEvent
      ? options.inactivityTimeoutMs ?? STREAM_INACTIVITY_DEADLINE_MS
      : Math.max(0, firstEventDeadlineAtMs - Date.now());
    const timeout = AbortSignal.timeout(timeoutMs);
    let abortTimeout = (): void => {};
    try {
      return await Promise.race([
        reader.read(),
        new Promise<never>((_, reject) => {
          abortTimeout = () =>
            reject(
              new ResponsesStreamError("Upstream Responses stream became inactive.", {
                kind: "inactivity_timeout",
              }),
            );
          timeout.addEventListener("abort", abortTimeout, { once: true });
        }),
      ]);
    } finally {
      timeout.removeEventListener("abort", abortTimeout);
    }
  };
  try {
    while (!terminal) {
      if (signal?.aborted) throw signal.reason;
      const { value, done } = await readWithDeadline();
      if (signal?.aborted) throw signal.reason;
      readerDone = done;
      if (value?.byteLength) {
        let segmentStart = 0;
        for (let index = 0; index < value.byteLength; index += 1) {
          const byte = value[index]!;
          ensurePendingSegmentWithinLimit(index - segmentStart + 1);
          if (!isEventBoundary(byte)) {
            advanceBoundaryState(byte);
            continue;
          }
          appendEventBytes(value.subarray(segmentStart, index + 1));
          segmentStart = index + 1;
          const parsed = parseEventBlock(takeEvent());
          if (!parsed) continue;
          markParsedEvent(parsed);
          yield parsed;
          if (parsed.terminal) return;
        }
        if (segmentStart < value.byteLength) appendEventBytes(value.subarray(segmentStart));
      }
      if (done) {
        const parsed = processTrailingEvent();
        if (parsed) {
          yield parsed;
          if (parsed.terminal) return;
        }
        throw new ResponsesStreamError("Upstream Responses stream ended before a terminal event.", {
          kind: "premature_eof",
        });
      }
    }
  } catch (error) {
    cancelReaderOnce(error);
    if (signal?.aborted) throw signal.reason;
    if (error instanceof ResponsesStreamError) throw error;
    throw new ResponsesStreamError("Upstream Responses stream could not be read.", { cause: error });
  } finally {
    signal?.removeEventListener("abort", abort);
    if (!terminal && !readerDone) cancelReaderOnce("Responses stream consumer stopped before a terminal event");
    reader.releaseLock();
  }
};

export const preflightResponsesStream = async (
  upstream: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
): Promise<PreflightedResponsesStream> => {
  const iterator = readResponsesStream(upstream, signal);
  try {
    const next = await iterator.next();
    if (next.done || !next.value) {
      throw new ResponsesStreamError("Upstream Responses stream ended before its first event.", {
        kind: "premature_eof",
      });
    }
    return {
      first: next.value,
      iterator,
    };
  } catch (error) {
    await iterator.return(error).catch(() => {});
    throw error;
  }
};

const errorEvent = (message: string): Uint8Array => {
  const value = {
    type: "error",
    error: {
      type: "server_error",
      code: "upstream_stream_error",
      message,
      param: null,
    },
  };
  return new TextEncoder().encode(`event: error\ndata: ${JSON.stringify(value)}\n\n`);
};

type ProxyResponsesStreamOptions = Readonly<{
  signal?: AbortSignal;
  downstreamSignal?: AbortSignal;
  onEvent?: (event: ResponsesStreamEvent) => void | Promise<void>;
  onFailure?: (error: unknown) => void | Promise<void>;
  onCancel?: (reason: unknown) => void | Promise<void>;
}>;

export const proxyResponsesStreamIterator = (
  iterator: ResponsesStreamIterator,
  options: ProxyResponsesStreamOptions = {},
  initialEvent?: ResponsesStreamEvent,
): ReadableStream<Uint8Array> => {
  const localAbort = new AbortController();
  let pending = initialEvent;
  let closed = false;
  const invoke = (callback: (() => void | Promise<void>) | undefined): void => {
    if (!callback) return;
    try {
      void Promise.resolve(callback()).catch(() => {});
    } catch {
      // Lifecycle callbacks must never interfere with downstream delivery.
    }
  };
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        const next = pending ? { done: false as const, value: pending } : await iterator.next();
        pending = undefined;
        if (closed) return;
        if (next.done) {
          closed = true;
          controller.close();
          return;
        }
        controller.enqueue(new TextEncoder().encode(next.value.raw));
        invoke(() => options.onEvent?.(next.value));
        if (next.value.terminal) {
          closed = true;
          controller.close();
          void iterator.return("Responses terminal event forwarded").catch(() => {});
        }
      } catch (error) {
        if (closed) return;
        if (!localAbort.signal.aborted && !options.signal?.aborted && !options.downstreamSignal?.aborted) {
          controller.enqueue(errorEvent("The upstream stream ended unexpectedly."));
        }
        closed = true;
        controller.close();
        invoke(() => options.onFailure?.(error));
      }
    },
    cancel(reason) {
      if (closed) return;
      closed = true;
      invoke(() => options.onCancel?.(reason));
      localAbort.abort(reason);
      void iterator.return(reason).catch(() => {});
    },
  });
};

export const proxyResponsesStream = (
  upstream: ReadableStream<Uint8Array>,
  options: ProxyResponsesStreamOptions = {},
): ReadableStream<Uint8Array> =>
  proxyResponsesStreamIterator(
    readResponsesStream(upstream, options.signal),
    options,
  );
