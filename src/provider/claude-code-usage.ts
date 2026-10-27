/**
 * Direct Claude subscription usage reads. Anthropic exposes the same OAuth
 * usage surface used by Claude Code's account panel; it is independent of an
 * inference request and therefore never spends a subscription turn. The
 * endpoint is undocumented, so this module owns its narrow wire contract,
 * bounded failures, and conversion into the gateway's existing quota windows.
 */
import { isRecord } from "../utils.ts";
import { parseClaudeCodeUsageSnapshot } from "./claude-code-usage-schema.ts";
import {
  CLAUDE_CODE_ANTHROPIC_VERSION,
  CLAUDE_CODE_BASE_URL,
  CLAUDE_CODE_OAUTH_BETA,
  type ClaudeCodeAuthHeaders,
  type ClaudeCodeFetch,
} from "./claude-code.ts";
import { sanitizeClaudeCodeErrorMessage, type ClaudeCodeQuotaObservation } from "./claude-code-quota.ts";

export const CLAUDE_CODE_USAGE_URL = `${CLAUDE_CODE_BASE_URL}/api/oauth/usage`;
/** The endpoint rate-limits unrecognized clients more aggressively. */
export const CLAUDE_CODE_USAGE_USER_AGENT = "claude-code/2.1.310";
const USAGE_TIMEOUT_MS = 8_000;
const ERROR_BODY_MAX_BYTES = 4_096;

export type ClaudeCodeUsageErrorKind =
  "scope_missing" | "auth_invalid" | "rate_limited" | "unsupported_credential" | "unreachable" | "upstream_error" | "http_error" | "invalid_response";

export type ClaudeCodeUsageSuccess = Readonly<{
  ok: true;
  status: number;
  observed_at_ms: number;
  observation: ClaudeCodeQuotaObservation;
}>;

export type ClaudeCodeUsageFailure = Readonly<{
  ok: false;
  kind: ClaudeCodeUsageErrorKind;
  status: number | null;
  observed_at_ms: number;
  message: string | null;
  retry_after_ms: number | null;
}>;

export type ClaudeCodeUsageResult = ClaudeCodeUsageSuccess | ClaudeCodeUsageFailure;

/** Direct meters retain their own complete snapshot; canonical guards stay restricted to 5h/7d. */
export const parseClaudeCodeUsagePayload = (payload: unknown, observedAtMs: number = Date.now()): ClaudeCodeQuotaObservation | null => {
  const snapshot = parseClaudeCodeUsageSnapshot(payload, Math.trunc(observedAtMs));
  if (!snapshot) return null;
  const windows: Record<string, { status: null; utilization: number | null; reset_at_ms: number | null; observed_at_ms: number }> = {};
  for (const [key, id] of [
    ["five_hour", "5h"],
    ["seven_day", "7d"],
  ] as const) {
    const window = snapshot.payload[key];
    const meter = snapshot.meters.find((item) => item.kind === key);
    if (window) {
      const reset = window.resets_at === null ? NaN : Date.parse(window.resets_at);
      windows[id] = {
        status: null,
        utilization: window.utilization === null ? null : Math.min(1, Math.max(0, window.utilization / 100)),
        reset_at_ms: meter?.reset_at_ms ?? (Number.isSafeInteger(reset) && reset > 0 ? reset : null),
        observed_at_ms: snapshot.observed_at_ms,
      };
    }
  }
  return { windows, standard: {}, representative_claim: null, observed_at_ms: snapshot.observed_at_ms, usage_snapshot: snapshot };
};

const boundedText = async (response: Response): Promise<string> => {
  const text = await response.text().catch(() => "");
  return text.length > ERROR_BODY_MAX_BYTES ? text.slice(0, ERROR_BODY_MAX_BYTES) : text;
};

const errorMessage = (text: string): string | null => {
  try {
    const payload: unknown = text ? JSON.parse(text) : null;
    if (isRecord(payload)) {
      if (isRecord(payload.error) && typeof payload.error.message === "string") return sanitizeClaudeCodeErrorMessage(payload.error.message);
      if (typeof payload.message === "string") return sanitizeClaudeCodeErrorMessage(payload.message);
    }
  } catch {
    // A non-JSON error body is still useful as bounded operator text.
  }
  return sanitizeClaudeCodeErrorMessage(text);
};

const retryAfterMs = (value: string | null, nowMs: number): number | null => {
  if (!value) return null;
  const seconds = Number(value);
  if (Number.isFinite(seconds) && seconds >= 0) return Math.trunc(seconds * 1_000);
  const at = Date.parse(value);
  return Number.isFinite(at) && at > 0 ? Math.max(0, at - nowMs) : null;
};

const failureKind = (status: number, message: string | null): ClaudeCodeUsageErrorKind => {
  if ((message && /user:profile|(?:missing|required|insufficient).*scope|scope.*(?:missing|required|insufficient)/i.test(message)) || status === 403)
    return "scope_missing";
  if (status === 401) return "auth_invalid";
  if (status === 429) return "rate_limited";
  if (status >= 500) return "upstream_error";
  return "http_error";
};

const failure = (
  kind: ClaudeCodeUsageErrorKind,
  status: number | null,
  observedAtMs: number,
  message: string | null,
  retryAfter: number | null = null
): ClaudeCodeUsageFailure => ({ ok: false, kind, status, observed_at_ms: observedAtMs, message, retry_after_ms: retryAfter });

/**
 * Read the subscription usage windows with the OAuth credential. Every failure
 * is returned as typed data so callers can distinguish an absent profile scope
 * from an upstream or transport failure without throwing or mutating auth.
 */
export const fetchClaudeCodeUsage = async (
  credential: ClaudeCodeAuthHeaders,
  options: Readonly<{ fetcher?: ClaudeCodeFetch; signal?: AbortSignal; now?: () => number }> = {}
): Promise<ClaudeCodeUsageResult> => {
  const clock = options.now ?? Date.now;
  if (credential.kind !== "oauth") {
    return failure(
      "unsupported_credential",
      null,
      Math.trunc(clock()),
      "Claude Console API keys do not expose the OAuth usage endpoint; gateway traffic continues to update the meter."
    );
  }
  const timeout = AbortSignal.timeout(USAGE_TIMEOUT_MS);
  const signal = options.signal ? AbortSignal.any([options.signal, timeout]) : timeout;
  const headers = new Headers({
    Authorization: `Bearer ${credential.secret}`,
    Accept: "application/json",
    "Content-Type": "application/json",
    "anthropic-beta": CLAUDE_CODE_OAUTH_BETA,
    "anthropic-version": CLAUDE_CODE_ANTHROPIC_VERSION,
    "User-Agent": CLAUDE_CODE_USAGE_USER_AGENT,
  });
  let response: Response;
  try {
    response = await (options.fetcher ?? fetch)(CLAUDE_CODE_USAGE_URL, { method: "GET", headers, redirect: "manual", signal });
  } catch (error) {
    const message = error instanceof Error ? sanitizeClaudeCodeErrorMessage(error.message) : null;
    return failure("unreachable", null, Math.trunc(clock()), message);
  }
  const observedAtMs = Math.trunc(clock());
  if (!response.ok) {
    const message = errorMessage(await boundedText(response));
    const kind = failureKind(response.status, message);
    return failure(
      kind,
      response.status,
      observedAtMs,
      kind === "scope_missing" && message === null ? "Claude OAuth credential cannot read usage because the user:profile scope is missing." : message,
      kind === "rate_limited" ? retryAfterMs(response.headers.get("retry-after"), observedAtMs) : null
    );
  }
  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    return failure("invalid_response", response.status, observedAtMs, "Claude usage returned an unreadable JSON payload.");
  }
  const observation = parseClaudeCodeUsagePayload(payload, observedAtMs);
  if (!observation) return failure("invalid_response", response.status, observedAtMs, "Claude usage returned an invalid or unrecognized meter payload.");
  return { ok: true, status: response.status, observed_at_ms: observedAtMs, observation };
};
