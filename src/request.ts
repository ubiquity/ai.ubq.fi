type RawBodyObserver = (bytes: Uint8Array<ArrayBuffer>) => void;
/** Reports why an accepted body never reached its observer, bounded to the fixed reasons. */
type RawBodyRejection = (reason: "body_over_limit" | "body_unavailable") => void;

const rawBodyObservers = new WeakMap<Request, Readonly<{ observer: RawBodyObserver; onRejected?: RawBodyRejection }>>();

export const MAX_ACCEPTED_JSON_BODY_BYTES = 32 * 1_024 * 1_024;

export type JsonBodyReadResult = Readonly<{ ok: true; value: unknown }> | Readonly<{ ok: false; kind: "empty" | "invalid" | "too_large" }>;

class RequestBodyTooLargeError extends Error {
  constructor() {
    super("Request body is too large");
    this.name = "RequestBodyTooLargeError";
  }
}

export const observeRawBodyOnce = (req: Request, observer: RawBodyObserver, onRejected?: RawBodyRejection): void => {
  rawBodyObservers.set(req, { observer, onRejected });
};

/** Transfer one accepted raw body to its observer under the fixed replay cap. */
export const captureRawBodyOnce = (req: Request, bytes: Uint8Array<ArrayBuffer>): boolean => {
  const pending = rawBodyObservers.get(req);
  rawBodyObservers.delete(req);
  if (!pending) return false;
  if (bytes.byteLength > MAX_ACCEPTED_JSON_BODY_BYTES) {
    // The route accepted this body for inference, but the fixed replay storage
    // contract cannot carry it: report the omission instead of staying silent.
    pending.onRejected?.("body_over_limit");
    return false;
  }
  pending.observer(bytes);
  return true;
};

export const discardRawBodyObserverOnce = (req: Request): void => {
  rawBodyObservers.delete(req);
};

/**
 * Report that the accepted body never reached its observer because the route
 * read itself stopped at the fixed cap. Returns whether an observer was
 * waiting, so a non-inference read stays a no-op.
 */
export const rejectRawBodyOnce = (req: Request, reason: "body_over_limit" | "body_unavailable"): boolean => {
  const pending = rawBodyObservers.get(req);
  rawBodyObservers.delete(req);
  if (!pending) return false;
  pending.onRejected?.(reason);
  return true;
};

const declaredContentLength = (req: Request): number | null => {
  const raw = req.headers.get("content-length");
  if (raw === null) return null;
  if (!/^(?:0|[1-9]\d*)$/.test(raw)) throw new Error("Request Content-Length is invalid");
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed)) throw new Error("Request Content-Length is invalid");
  return parsed;
};

const readBoundedRequestBody = async (req: Request, maxBytes: number): Promise<Uint8Array<ArrayBuffer>> => {
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

export const readJsonBodyWithLimit = async (req: Request, maxBytes: number): Promise<JsonBodyReadResult> => {
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  let captured = false;
  try {
    bytes = await readBoundedRequestBody(req, maxBytes);
    if (bytes.byteLength === 0) return { ok: false, kind: "empty" };
    const parsed: unknown = JSON.parse(new TextDecoder().decode(bytes));
    // Replay capture has its own fixed 32 MiB storage contract even when a
    // route permits a larger JSON body for protocol-specific payloads.
    captured = captureRawBodyOnce(req, bytes);
    return { ok: true, value: parsed };
  } catch (error) {
    // A body the route refused at its own fixed cap is an explicit omission
    // reason, not a silently empty capture for the request that failed here.
    if (error instanceof RequestBodyTooLargeError) rejectRawBodyOnce(req, "body_over_limit");
    return { ok: false, kind: error instanceof RequestBodyTooLargeError ? "too_large" : "invalid" };
  } finally {
    discardRawBodyObserverOnce(req);
    if (bytes && !captured) bytes.fill(0);
  }
};

export const readJsonBody = async (req: Request, maxBytes = MAX_ACCEPTED_JSON_BODY_BYTES): Promise<unknown> => {
  const result = await readJsonBodyWithLimit(req, maxBytes);
  return result.ok ? result.value : null;
};
