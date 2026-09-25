/**
 * The declared, versioned policy that bounds how much payload this gateway
 * forwards inside one Chat Completions message.
 *
 * Why this exists: both providers on this translation count forwarded bytes -
 * base64 image data URLs and raw tool results alike - as text tokens. On
 * 2026-09-24 a single 744,586-byte `view_image` result took one session from
 * 736,213 requested tokens to 1,250,713 against a 1,048,576-token window; every
 * later request replayed the same blob and was rejected, so the thread could
 * not be resumed.
 *
 * The first stopgap cut each payload at an undeclared 64 KiB constant. This
 * policy replaces it: the limit is declared here, advertised to Codex clients
 * through the model catalog's `forwarding_policy` extension, carried in every
 * elision marker and operator log line, and versioned so a change is visible
 * rather than silent.
 *
 * - `mode: "bytes"` counts the UTF-8 bytes of one forwarded message payload.
 * - `perMessageLimit` bounds the forwarded payload including its elision marker.
 * - Reduction is deterministic (a byte prefix plus a marker naming the counts)
 *   and applies only when the request did not forbid it: an explicit
 *   `truncation: "disabled"` fails closed with HTTP 400
 *   `context_length_exceeded` instead of mutating the input.
 * - Aggregate request admission (the whole rendered prompt against the model
 *   window) is not covered by this policy; it stays a separate control.
 */
export const FORWARDED_PAYLOAD_POLICY = {
  version: "deepseek-forwarded-payload/v1",
  mode: "bytes",
  perMessageLimit: 65_536,
} as const;

/** The reduction decision one request authorizes for oversized forwarded payloads. */
export type ForwardedPayloadReduction = "reduce" | "reject";

/** One payload the declared policy reduced, as recorded in the operator log. */
export type ForwardedPayloadElision = Readonly<{
  path: string;
  callId: string | null;
  kind: "tool_output" | "image";
  originalBytes: number;
  forwardedBytes: number;
  omittedBytes: number;
}>;
