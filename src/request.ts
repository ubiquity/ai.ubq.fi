type RawJsonBodyObserver = (bytes: Uint8Array<ArrayBuffer>) => void;

const rawJsonBodyObservers = new WeakMap<Request, RawJsonBodyObserver>();

export const MAX_ACCEPTED_JSON_BODY_BYTES = 32 * 1_024 * 1_024;

export const observeRawJsonBodyOnce = (req: Request, observer: RawJsonBodyObserver): void => {
  rawJsonBodyObservers.set(req, observer);
};

const declaredContentLength = (req: Request): number | null => {
  const raw = req.headers.get("content-length");
  if (raw === null) return null;
  if (!/^(?:0|[1-9][0-9]*)$/.test(raw)) throw new Error("Request Content-Length is invalid");
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed)) throw new Error("Request Content-Length is invalid");
  return parsed;
};

const readBoundedRequestBody = async (req: Request): Promise<Uint8Array<ArrayBuffer>> => {
  const declared = declaredContentLength(req);
  if (declared !== null && declared > MAX_ACCEPTED_JSON_BODY_BYTES) {
    await req.body?.cancel().catch(() => {});
    throw new Error("Request body is too large");
  }
  if (!req.body) return new Uint8Array();
  const reader = req.body.getReader();
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength > MAX_ACCEPTED_JSON_BODY_BYTES - total) {
        await reader.cancel().catch(() => {});
        throw new Error("Request body is too large");
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

export const readJsonBody = async (req: Request): Promise<unknown> => {
  const observer = rawJsonBodyObservers.get(req);
  rawJsonBodyObservers.delete(req);
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  let captured = false;
  try {
    bytes = await readBoundedRequestBody(req);
    const parsed: unknown = JSON.parse(new TextDecoder().decode(bytes));
    if (observer) {
      observer(bytes);
      captured = true;
    }
    return parsed;
  } catch {
    return null;
  } finally {
    if (bytes && !captured) bytes.fill(0);
  }
};
