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

export class ResponsesStreamError extends Error {
  constructor(message: string, options?: ErrorOptions) {
    super(message, options);
    this.name = "ResponsesStreamError";
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
    throw new ResponsesStreamError("Upstream emitted malformed Responses SSE JSON.", { cause });
  }
  if (!isRecord(value)) {
    throw new ResponsesStreamError("Upstream emitted a non-object Responses SSE event.");
  }
  const type = getString(value.type)?.trim();
  if (!type) throw new ResponsesStreamError("Upstream emitted a Responses SSE event without a type.");
  return { raw, value, type, terminal: RESPONSES_TERMINAL_EVENT_TYPES.has(type) };
};

export const readResponsesStream = async function* (
  stream: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
): AsyncGenerator<ResponsesStreamEvent> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let terminal = false;
  const abort = () => void reader.cancel(signal?.reason).catch(() => {});
  signal?.addEventListener("abort", abort, { once: true });
  try {
    while (!terminal) {
      if (signal?.aborted) throw signal.reason;
      const { value, done } = await reader.read();
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
          await reader.cancel("Responses terminal event received").catch(() => {});
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
              await reader.cancel("Responses terminal event received").catch(() => {});
            }
            yield parsed;
            if (parsed.terminal) return;
          }
        }
        throw new ResponsesStreamError("Upstream Responses stream ended before a terminal event.");
      }
    }
  } catch (error) {
    if (error instanceof ResponsesStreamError || signal?.aborted) throw error;
    throw new ResponsesStreamError("Upstream Responses stream could not be read.", { cause: error });
  } finally {
    signal?.removeEventListener("abort", abort);
    reader.releaseLock();
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

export const proxyResponsesStream = (
  upstream: ReadableStream<Uint8Array>,
  options: Readonly<{
    signal?: AbortSignal;
    onEvent?: (event: ResponsesStreamEvent) => void | Promise<void>;
    onFailure?: (error: unknown) => void | Promise<void>;
  }> = {},
): ReadableStream<Uint8Array> => {
  const localAbort = new AbortController();
  const signal = options.signal ? AbortSignal.any([options.signal, localAbort.signal]) : localAbort.signal;
  const iterator = readResponsesStream(upstream, signal);
  let closed = false;
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        const next = await iterator.next();
        if (next.done) {
          closed = true;
          controller.close();
          return;
        }
        await options.onEvent?.(next.value);
        controller.enqueue(new TextEncoder().encode(next.value.raw));
        if (next.value.terminal) {
          await iterator.return("Responses terminal event forwarded").catch(() => {});
          closed = true;
          controller.close();
        }
      } catch (error) {
        await options.onFailure?.(error);
        if (!options.signal?.aborted) controller.enqueue(errorEvent("The upstream stream ended unexpectedly."));
        closed = true;
        controller.close();
      }
    },
    async cancel(reason) {
      closed = true;
      localAbort.abort(reason);
      await iterator.return(reason).catch(() => {});
    },
  });
};
