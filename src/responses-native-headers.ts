import { earlySseHeadersCommitted, tryCommitEarlySseHeaders } from "#http";
import { isCodexNativeRequest } from "./deepseek/context-admission-failure.ts";
import { withCors } from "./http.ts";
import { downstreamSignalFor } from "./openai.ts";
import { recordStreamTerminal, type UsageContext } from "./openai-telemetry.ts";
import { cerebrasUpstreamModelFor } from "./provider/cerebras.ts";
import { isRecord } from "./utils.ts";
import { readBoundedResponseBody } from "./bounded-response-body.ts";
import { SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS, SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES } from "./sentinel/upstream-capture.ts";

type NativeHttpFailure = Readonly<{
  status: number;
  headers: Readonly<Record<string, string>>;
  headers_truncated: boolean;
  error: unknown;
  body_complete: boolean;
}>;
const originalFailures = new WeakMap<Request, NativeHttpFailure>();

/** Original Request identity survives response/CORS/telemetry wrappers. No extra wire fields. */
export const originalNativeHttpFailure = (request: Request | undefined): NativeHttpFailure | undefined => (request ? originalFailures.get(request) : undefined);

const safeFailureHeaders = (source: Headers): Pick<NativeHttpFailure, "headers" | "headers_truncated"> => {
  const headers: Record<string, string> = {};
  let truncated = false;
  for (const name of SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES) {
    const value = source.get(name);
    if (value === null) continue;
    headers[name] = value.slice(0, SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS);
    truncated ||= value.length > SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS;
  }
  return { headers, headers_truncated: truncated };
};

/** Called only after shared validation; provider-local validation still precedes its dispatch hook. */
export const nativeHeaderUsageContext = (req: Request, raw: Readonly<Record<string, unknown>>, context: UsageContext | undefined): UsageContext | undefined => {
  const model = typeof raw.model === "string" ? raw.model.trim() : "";
  if (!context || raw.stream !== true || !isCodexNativeRequest(req) || cerebrasUpstreamModelFor(model)) return context;
  const headers = new Headers({ "content-type": "text/event-stream", "cache-control": "no-store" });
  if (context.requestId) headers.set("x-uos-request-id", context.requestId);
  const knownHeaders = withCors(new Response(null, { headers }), req).headers;
  const beforeDispatch = context.beforeProviderDispatch;
  return {
    ...context,
    beforeProviderDispatch: async (provider) => {
      // Keyless admission already has no API-key hook. Preserve that existing seam.
      const receipt = await beforeDispatch?.(provider);
      if (!downstreamSignalFor(req, context).aborted) tryCommitEarlySseHeaders(req, { headers: knownHeaders });
      return receipt;
    },
  };
};

/** Only a selected late HTTP failure changes encoding; all provider selection has already finished. */
export const nativeFailureAfterEarlyHeaders = async (req: Request, response: Response, context: UsageContext): Promise<Response> => {
  if (response.ok || /^text\/event-stream(?:\s*;|$)/i.test(response.headers.get("content-type") ?? "") || !earlySseHeadersCommitted(req) || req.signal.aborted)
    return response;
  const captured = await readBoundedResponseBody(response, { signal: req.signal, cancellationReason: "Selected native HTTP error body incomplete" });
  const text = captured.complete ? new TextDecoder().decode(captured.bytes) : "";
  let payload: unknown;
  try {
    payload = JSON.parse(text);
  } catch {
    payload = null;
  }
  const originalError: unknown =
    isRecord(payload) && isRecord(payload.error) ? payload.error : { message: text || `Upstream request failed with HTTP ${response.status}.` };
  const error = isRecord(originalError) ? originalError : {};
  const code = typeof error.code === "string" ? error.code : "server_error";
  const message = typeof error.message === "string" ? error.message : `Upstream request failed with HTTP ${response.status}.`;
  const id = `resp_${crypto.randomUUID().replaceAll("-", "")}`;
  const envelope = { id, object: "response", created_at: Math.floor(Date.now() / 1000), status: "in_progress", error: null, output: [], usage: null };
  const events = [
    { type: "response.created", sequence_number: 0, response: envelope },
    { type: "response.failed", sequence_number: 1, response: { ...envelope, status: "failed", error: { code, message } } },
  ];
  const result = new Response(events.map((event) => `event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`).join(""), {
    headers: { "content-type": "text/event-stream", "cache-control": "no-store" },
  });
  originalFailures.set(req, {
    status: response.status,
    ...safeFailureHeaders(response.headers),
    error: captured.complete ? originalError : null,
    body_complete: captured.complete,
  });
  const telemetry = context.responseTelemetry;
  if (telemetry) {
    telemetry.completed = false;
    telemetry.semanticOutputObserved = false;
    telemetry.stream = true;
    telemetry.streamTerminalType = "response.failed";
    telemetry.failureKind ??= code;
    telemetry.syntheticTerminalType = "response.failed";
    telemetry.responseCreatedObserved = true;
    recordStreamTerminal(context);
  }
  return result;
};
