import { getString, isRecord } from "./utils.ts";

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

export type ResponsesStreamFailureKind = "malformed_event" | "premature_eof" | "read_error";

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

const eventBoundary = (buffer: string): { index: number; length: number } | null => {
  const match = /\r\n\r\n|\n\n|\r\r/.exec(buffer);
  return match ? { index: match.index, length: match[0].length } : null;
};

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
): ResponsesStreamIterator {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
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
  try {
    while (!terminal) {
      if (signal?.aborted) throw signal.reason;
      const { value, done } = await reader.read();
      if (signal?.aborted) throw signal.reason;
      readerDone = done;
      buffer += decoder.decode(value, { stream: !done });

      while (true) {
        const boundary = eventBoundary(buffer);
        if (!boundary) break;
        const end = boundary.index + boundary.length;
        const parsed = parseEventBlock(buffer.slice(0, end));
        buffer = buffer.slice(end);
        if (!parsed) continue;
        if (parsed.terminal) {
          terminal = true;
          cancelReaderOnce("Responses terminal event received");
        }
        yield parsed;
        if (parsed.terminal) return;
      }
      if (done) {
        if (buffer.trim()) {
          const parsed = parseEventBlock(buffer);
          buffer = "";
          if (parsed) {
            if (parsed.terminal) {
              terminal = true;
              cancelReaderOnce("Responses terminal event received");
            }
            yield parsed;
            if (parsed.terminal) return;
          }
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
