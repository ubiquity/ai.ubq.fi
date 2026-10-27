import type { ResponsesStreamEvent } from "../responses-stream.ts";
import { getString, isRecord } from "../utils.ts";

/** Codex ignores generic context errors; keep its response failure classification visible. */
export const codexContextFailureEvent = (
  event: ResponsesStreamEvent,
  responseId: string | null,
  responseTemplate: Readonly<Record<string, unknown>>,
  visibleOutput: readonly Record<string, unknown>[]
): ResponsesStreamEvent => {
  if (event.type !== "error" || !responseId) return event;
  const nested = event.value.error;
  const error =
    isRecord(nested) && !Array.isArray(nested)
      ? nested
      : Object.fromEntries(Object.entries(event.value).filter(([key]) => key !== "type" && key !== "sequence_number"));
  if (error.code !== "context_length_exceeded") return event;
  const output = Array.isArray(responseTemplate.output) ? [...responseTemplate.output] : [];
  for (const item of visibleOutput) {
    const id = getString(item.id);
    const index = id ? output.findIndex((existing) => isRecord(existing) && existing.id === id) : -1;
    if (index < 0) output.push(item);
    else output[index] = item;
  }
  const value = {
    type: "response.failed",
    ...(event.value.sequence_number === undefined ? {} : { sequence_number: event.value.sequence_number }),
    response: {
      ...responseTemplate,
      id: responseId,
      object: getString(responseTemplate.object) ?? "response",
      status: "failed",
      error: { ...error },
      output,
    },
  };
  return { type: value.type, terminal: true, value, raw: `event: response.failed\ndata: ${JSON.stringify(value)}\n\n` };
};
