type RawBodyObserver = (bytes: Uint8Array<ArrayBuffer>) => void;

const rawBodyObservers = new WeakMap<Request, RawBodyObserver>();

export const MAX_ACCEPTED_JSON_BODY_BYTES = 32 * 1_024 * 1_024;

export type JsonBodyReadResult =
  | Readonly<{ ok: true; value: unknown }>
  | Readonly<{ ok: false; kind: "invalid" | "too_large" }>;

class RequestBodyTooLargeError extends Error {
  constructor() {
    super("Request body is too large");
    this.name = "RequestBodyTooLargeError";
  }
}

export const observeRawBodyOnce = (req: Request, observer: RawBodyObserver): void => {
  rawBodyObservers.set(req, observer);
};

/** Transfer one accepted raw body to its observer under the fixed replay cap. */
export const captureRawBodyOnce = (req: Request, bytes: Uint8Array<ArrayBuffer>): boolean => {
  const observer = rawBodyObservers.get(req);
  rawBodyObservers.delete(req);
  if (!observer || bytes.byteLength > MAX_ACCEPTED_JSON_BODY_BYTES) return false;
  observer(bytes);
  return true;
};

export const discardRawBodyObserverOnce = (req: Request): void => {
  rawBodyObservers.delete(req);
};

const declaredContentLength = (req: Request): number | null => {
  const raw = req.headers.get("content-length");
  if (raw === null) return null;
  if (!/^(?:0|[1-9][0-9]*)$/.test(raw)) throw new Error("Request Content-Length is invalid");
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed)) throw new Error("Request Content-Length is invalid");
  return parsed;
};

const readBoundedRequestBody = async (
  req: Request,
  maxBytes: number,
): Promise<Uint8Array<ArrayBuffer>> => {
  const declared = declaredContentLength(req);
  if (declared !== null && declared > maxBytes) {
    await req.body?.cancel().catch(() => {});
    throw new RequestBodyTooLargeError();
  }
  if (!req.body) return new Uint8Array();
  const reader = req.body.getReader();
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength > maxBytes - total) {
        await reader.cancel().catch(() => {});
        throw new RequestBodyTooLargeError();
      }
      total += value.byteLength;
      chunks.push(new Uint8Array(value));
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
      chunk.fill(0);
    }
    return bytes;
  } catch (error) {
    for (const chunk of chunks) chunk.fill(0);
    await reader.cancel(error).catch(() => {});
    throw error;
  } finally {
    reader.releaseLock();
  }
};

export const readJsonBodyWithLimit = async (
  req: Request,
  maxBytes: number,
): Promise<JsonBodyReadResult> => {
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  let captured = false;
  try {
    bytes = await readBoundedRequestBody(req, maxBytes);
    const parsed: unknown = JSON.parse(new TextDecoder().decode(bytes));
    // Replay capture has its own fixed 32 MiB storage contract even when a
    // route permits a larger JSON body for protocol-specific payloads.
    captured = captureRawBodyOnce(req, bytes);
    return { ok: true, value: parsed };
  } catch (error) {
    return { ok: false, kind: error instanceof RequestBodyTooLargeError ? "too_large" : "invalid" };
  } finally {
    discardRawBodyObserverOnce(req);
    if (bytes && !captured) bytes.fill(0);
  }
};

export const readJsonBody = async (
  req: Request,
  maxBytes = MAX_ACCEPTED_JSON_BODY_BYTES,
): Promise<unknown> => {
  const result = await readJsonBodyWithLimit(req, maxBytes);
  return result.ok ? result.value : null;
};
