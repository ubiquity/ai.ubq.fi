/**
 * Error types owned by the m03 baseline adapter module.
 *
 * The module keeps three orthogonal failure kinds so callers (and the
 * runner's adapter_error classification) can distinguish configuration gaps
 * from deterministic policy rejections and upstream failures:
 *
 * - {@link BaselineNotProvisionedError}: the adapter was never configured to
 *   make live calls (no approved model, no pinned checkout, no injected
 *   transport). This is the safe default for A/B/D; nothing runs.
 * - {@link BaselineAdapterError}: deterministic local rejection (invalid
 *   configuration, unparseable model output, protocol-policy mirror of the
 *   gateway).
 * - {@link BaselineUpstreamError}: the live transport returned a non-2xx
 *   response; only status plus a sanitized provider code/message is carried
 *   (never request bodies, keys or private reasoning).
 */

export type BaselineAdapterErrorCode =
  | "not-provisioned"
  | "invalid-config"
  | "invalid-upstream-response"
  | "unproven-format"
  | "request-limit"
  | "bridge-parse";

export class BaselineAdapterError extends Error {
  readonly code: BaselineAdapterErrorCode;

  constructor(message: string, code: BaselineAdapterErrorCode) {
    super(message);
    this.name = "BaselineAdapterError";
    this.code = code;
  }
}

/** Raised when a baseline is asked to run without live configuration. */
export class BaselineNotProvisionedError extends BaselineAdapterError {
  constructor(message: string) {
    super(message, "not-provisioned");
    this.name = "BaselineNotProvisionedError";
  }
}

/** Summary of a failed upstream call: status + sanitized provider code/message. */
export class BaselineUpstreamError extends Error {
  readonly status: number;
  readonly providerCode: string | null;
  readonly providerMessage: string | null;

  constructor(status: number, providerCode: string | null, providerMessage: string | null) {
    const parts: string[] = [`upstream request failed with status ${status}`];
    if (providerCode) parts.push(`code=${providerCode}`);
    if (providerMessage) parts.push(`message=${JSON.stringify(providerMessage)}`);
    super(parts.join(" "));
    this.name = "BaselineUpstreamError";
    this.status = status;
    this.providerCode = providerCode;
    this.providerMessage = providerMessage;
  }
}

/** Extracts a bounded code/message pair from common OpenAI error payloads. */
export function sanitizedUpstreamError(body: unknown): { code: string | null; message: string | null } {
  if (typeof body !== "object" || body === null || Array.isArray(body)) return { code: null, message: null };
  const record = body as Record<string, unknown>;
  const code = typeof record.code === "string" ? record.code : null;
  const message = typeof record.message === "string" ? record.message : null;
  if (code !== null || message !== null) return { code, message };
  const inner = record.error;
  if (typeof inner === "object" && inner !== null && !Array.isArray(inner)) {
    const error = inner as Record<string, unknown>;
    return {
      code: typeof error.code === "string" ? error.code : null,
      message: typeof error.message === "string" ? error.message : null,
    };
  }
  return { code: null, message: null };
}
