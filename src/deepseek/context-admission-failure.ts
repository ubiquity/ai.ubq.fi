import { parseTurnMetadata } from "../../lib/jev_compaction/codex_items.ts";
import { openaiError } from "../http.ts";
import { attachResponseTelemetry, createResponseTelemetryState, recordTerminalUsage, type UsageContext } from "../openai-telemetry.ts";
import type { DeepSeekResponsesFailure } from "./responses.ts";

const localContextFailures = new WeakSet<Response>();

/** Encoding selection only: this metadata grants no authority or validation bypass. */
export const isCodexNativeRequest = (req: Request): boolean => {
  const metadata = parseTurnMetadata(req.headers.get("x-codex-turn-metadata"));
  return metadata !== null && ["session_id", "thread_id", "turn_id"].every((key) => typeof metadata[key] === "string" && metadata[key].trim().length > 0);
};

/** A local request failure must never become a retryable provider-waterfall failure. */
export const isLocalContextAdmissionFailure = (response: Response): boolean => localContextFailures.has(response);

/** Keeps public HTTP errors unchanged and gives native Codex its typed context recovery signal. */
export const chatResponsesProjectionFailure = (
  req: Request,
  rawRecord: Readonly<Record<string, unknown>>,
  failure: DeepSeekResponsesFailure,
  context?: UsageContext
): Response => {
  if (rawRecord.stream !== true || failure.kind !== "aggregate_context_overflow" || !isCodexNativeRequest(req)) {
    return openaiError(400, failure.message, failure.code ?? "invalid_request_error", { param: failure.param });
  }
  const id = `resp_${crypto.randomUUID().replaceAll("-", "")}`;
  const response = { id, object: "response", created_at: Math.floor(Date.now() / 1000), status: "in_progress", error: null, output: [], usage: null };
  const events = [
    { type: "response.created", sequence_number: 0, response },
    {
      type: "response.failed",
      sequence_number: 1,
      response: { ...response, status: "failed", error: { code: failure.code, message: failure.message, param: failure.param } },
    },
  ];
  const body = events.map((event) => `event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`).join("");
  const result = new Response(body, { status: 200, headers: { "content-type": "text/event-stream", "cache-control": "no-store" } });
  const telemetry = context?.responseTelemetry ?? createResponseTelemetryState();
  telemetry.completed = false;
  telemetry.semanticOutputObserved = false;
  telemetry.stream = true;
  telemetry.streamTerminalType = "response.failed";
  telemetry.failureKind = "aggregate_context_overflow";
  telemetry.syntheticTerminalType = "response.failed";
  telemetry.responseCreatedObserved = true;
  recordTerminalUsage(context, null, false);
  localContextFailures.add(result);
  return attachResponseTelemetry(result, telemetry);
};
