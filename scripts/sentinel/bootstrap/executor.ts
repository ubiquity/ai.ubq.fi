// Protected GitHub Actions executor-identity verifier (provider owner
// handover). This module is part of the bootstrap trust domain: it verifies
// exactly the two recorded run-attempt endpoints with the injected fetcher
// and the fixed https://api.github.com API header style. It never substitutes
// a latest-run or listing endpoint, never scans prior history, never calls
// execution/promotion APIs, and never confers promotion authority. Every
// failure throws a safe ExecutorAuthorityError carrying only a fixed
// field/status label — never credentials or untrusted response bytes.

import type { SentinelProviderExecutorV1, SentinelProviderStopConclusion } from "./provider-state.ts";

export const PROVIDER_EXECUTOR_REPOSITORY = "ubiquity/ai.ubq.fi" as const;
export const PROVIDER_EXECUTOR_WORKFLOW_PATH = ".github/workflows/sentinel-revision-control.yml" as const;

const API_BASE = "https://api.github.com";
const RUN_URL_BASE = `https://github.com/${PROVIDER_EXECUTOR_REPOSITORY}/actions/runs`;
const RESPONSE_BOUND_BYTES = 1024 * 1024;

/** Real terminal conclusions only; synthetic stale and null/unknown are excluded. */
const TERMINAL_CONCLUSIONS = [
  "success",
  "failure",
  "cancelled",
  "timed_out",
  "action_required",
  "neutral",
  "skipped",
] as const;

/** GitHub UTC timestamps: seconds or 1..3 fractional digits, Z terminator. */
const GITHUB_TIMESTAMP = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/u;

type JsonRecord = Record<string, unknown>;

const record = (value: unknown): JsonRecord | null =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? value as JsonRecord : null;

export class ExecutorAuthorityError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ExecutorAuthorityError";
  }
}

/**
 * Strict local executor identity: exactly the approved repository and the
 * exact revision-control workflow with positive safe integer run id/attempt.
 * Returns null for anything else; the caller rejects before any execution
 * API call. Never infers legacy authorization from permissive schema values.
 */
export const parseProviderExecutorIdentity = (value: unknown): SentinelProviderExecutorV1 | null => {
  const candidate = record(value);
  if (candidate === null) return null;
  const keys = Object.keys(candidate);
  if (
    keys.length !== 4 ||
    !keys.every(
      (key) =>
        key === "repository" || key === "workflow_path" || key === "run_id" ||
        key === "run_attempt",
    )
  ) return null;
  if (candidate.repository !== PROVIDER_EXECUTOR_REPOSITORY) return null;
  if (candidate.workflow_path !== PROVIDER_EXECUTOR_WORKFLOW_PATH) return null;
  const runId = candidate.run_id;
  const runAttempt = candidate.run_attempt;
  if (typeof runId !== "number" || !Number.isSafeInteger(runId) || runId <= 0) return null;
  if (typeof runAttempt !== "number" || !Number.isSafeInteger(runAttempt) || runAttempt <= 0) {
    return null;
  }
  return Object.freeze({
    repository: PROVIDER_EXECUTOR_REPOSITORY,
    workflow_path: PROVIDER_EXECUTOR_WORKFLOW_PATH,
    run_id: runId,
    run_attempt: runAttempt,
  });
};

/** Validates a GitHub UTC timestamp and returns the normalized ISO UTC form. */
const normalizedTimestamp = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !GITHUB_TIMESTAMP.test(value)) {
    throw new ExecutorAuthorityError(`${label} timestamp is invalid`);
  }
  const parsed = Date.parse(value);
  if (!Number.isFinite(parsed)) throw new ExecutorAuthorityError(`${label} timestamp is invalid`);
  const normalized = new Date(parsed).toISOString();
  // Round-trip the calendar part: invalid dates (e.g. February 30) normalize
  // to a different instant and must fail closed.
  if (normalized.slice(0, 19) !== value.slice(0, 19)) {
    throw new ExecutorAuthorityError(`${label} timestamp is not a valid UTC instant`);
  }
  return normalized;
};

/** Validates an injected clock value as a finite canonical UTC instant. */
const normalizeClock = (value: unknown, label: string): string => {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new ExecutorAuthorityError(`${label} clock is not a finite timestamp`);
  }
  // A finite but out-of-range number constructs an invalid Date whose
  // toISOString would throw a raw RangeError; check the instant first so
  // every clock violation fails closed as a safe authority error.
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    throw new ExecutorAuthorityError(`${label} clock is not a valid UTC instant`);
  }
  const normalized = parsed.toISOString();
  if (Date.parse(normalized) !== value) {
    throw new ExecutorAuthorityError(`${label} clock is not a valid UTC instant`);
  }
  return normalized;
};

/**
 * Reads the response body through its reader while enforcing the 1 MiB raw
 * bound incrementally: only in-bound chunks are accumulated, an overflow
 * cancels the stream, and decoding is strict fatal UTF-8 so malformed bytes
 * are never silently changed. Returns the retained raw text.
 */
const readBoundedBody = async (response: Response): Promise<string> => {
  const body = response.body;
  if (body === null) throw new ExecutorAuthorityError("executor authority response has no body");
  const decoder = new TextDecoder("utf-8", { fatal: true });
  const reader = body.getReader();
  let totalBytes = 0;
  let text = "";
  while (true) {
    let chunk: Uint8Array;
    try {
      const next = await reader.read();
      if (next.done) break;
      chunk = next.value;
    } catch {
      throw new ExecutorAuthorityError("executor authority response body is unreadable");
    }
    totalBytes += chunk.byteLength;
    if (totalBytes > RESPONSE_BOUND_BYTES) {
      await reader.cancel().catch(() => undefined);
      throw new ExecutorAuthorityError("executor authority response body exceeds 1 MiB");
    }
    try {
      text += decoder.decode(chunk, { stream: true });
    } catch {
      await reader.cancel().catch(() => undefined);
      throw new ExecutorAuthorityError("executor authority response body is not valid UTF-8");
    }
  }
  try {
    text += decoder.decode();
  } catch {
    throw new ExecutorAuthorityError("executor authority response body is not valid UTF-8");
  }
  return text;
};

const attemptsContainToken = (payload: unknown, token: string): boolean =>
  typeof token === "string" && token !== "" &&
  JSON.stringify(payload).includes(token);

const boundHtmlUrl = (value: unknown, runId: number, runAttempt: number): string => {
  if (typeof value !== "string") throw new ExecutorAuthorityError("html_url is invalid");
  const runUrl = `${RUN_URL_BASE}/${runId}`;
  if (value === runUrl) return value;
  if (value === `${runUrl}/attempts/${runAttempt}`) return value;
  throw new ExecutorAuthorityError("html_url is not bound to the exact run");
};

export type VerifiedAttempt = Readonly<{
  request_path: string;
  http_status: 200;
  /** Original parsed JSON response, retained verbatim as raw evidence. */
  response: JsonRecord;
  run_id: number;
  run_attempt: number;
  html_url: string;
  status: string;
  conclusion: string | null;
  /** Normalized ISO UTC operational timestamps. */
  created_at: string;
  run_started_at: string;
  updated_at: string;
}>;

export type VerifiedRetiringAttempt =
  & VerifiedAttempt
  & Readonly<{ status: "completed"; conclusion: SentinelProviderStopConclusion }>;

export type VerifiedReplacementAttempt =
  & VerifiedAttempt
  & Readonly<{ status: "in_progress"; conclusion: null }>;

export type VerifiedExecutorAuthority = Readonly<{
  observed_at: string;
  retiring: VerifiedRetiringAttempt;
  next: VerifiedReplacementAttempt;
}>;

export type VerifiedActiveExecutorAuthority = Readonly<{
  observed_at: string;
  attempt: VerifiedReplacementAttempt;
}>;

const verifyAttempt = async (
  input: Readonly<{
    token: string;
    fetcher: typeof fetch;
    executor: SentinelProviderExecutorV1;
  }>,
): Promise<VerifiedAttempt> => {
  if (
    input.executor.repository !== PROVIDER_EXECUTOR_REPOSITORY ||
    input.executor.workflow_path !== PROVIDER_EXECUTOR_WORKFLOW_PATH
  ) throw new ExecutorAuthorityError("executor workflow identity is not approved");
  const requestPath =
    `/repos/${PROVIDER_EXECUTOR_REPOSITORY}/actions/runs/${input.executor.run_id}/attempts/${input.executor.run_attempt}`;
  const url = `${API_BASE}${requestPath}`;
  let response: Response;
  try {
    response = await input.fetcher(url, {
      method: "GET",
      redirect: "error",
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${input.token}`,
        "X-GitHub-Api-Version": "2022-11-28",
      },
    });
  } catch {
    throw new ExecutorAuthorityError("executor authority transport failed");
  }
  if (response.status !== 200) {
    throw new ExecutorAuthorityError(`executor authority response is not HTTP 200 (status ${response.status})`);
  }
  // Streamed bound: no unbounded body is ever buffered; overflow cancels.
  const text = await readBoundedBody(response);
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    throw new ExecutorAuthorityError("executor authority response is not valid JSON");
  }
  const payload = record(parsed);
  if (payload === null) throw new ExecutorAuthorityError("executor authority response is not a JSON object");
  // Reject the exact nonempty token both in the raw bytes and in the parsed
  // JSON (keys/string values via its canonical serialization), so a Unicode
  // escaped credential cannot be decoded and silently retained.
  if (
    input.token !== "" &&
    (text.includes(input.token) || attemptsContainToken(payload, input.token))
  ) {
    throw new ExecutorAuthorityError("executor authority response echoed the supplied token");
  }
  if (payload === null) throw new ExecutorAuthorityError("executor authority response is not a JSON object");
  if (payload.id !== input.executor.run_id) {
    throw new ExecutorAuthorityError("executor authority response id does not match the run");
  }
  if (payload.run_attempt !== input.executor.run_attempt) {
    throw new ExecutorAuthorityError("executor authority response run_attempt does not match");
  }
  const repository = record(payload.repository);
  if (repository === null || repository.full_name !== PROVIDER_EXECUTOR_REPOSITORY) {
    throw new ExecutorAuthorityError("executor authority response repository identity is invalid");
  }
  if (payload.path !== PROVIDER_EXECUTOR_WORKFLOW_PATH) {
    throw new ExecutorAuthorityError("executor authority response workflow path is invalid");
  }
  if (typeof payload.status !== "string") {
    throw new ExecutorAuthorityError("executor authority response status is invalid");
  }
  if (payload.conclusion !== null && typeof payload.conclusion !== "string") {
    throw new ExecutorAuthorityError("executor authority response conclusion is invalid");
  }
  const conclusion = payload.conclusion as string | null;
  const created = normalizedTimestamp(payload.created_at, "created_at");
  const started = normalizedTimestamp(payload.run_started_at, "run_started_at");
  const updated = normalizedTimestamp(payload.updated_at, "updated_at");
  if (Date.parse(created) > Date.parse(started) || Date.parse(started) > Date.parse(updated)) {
    throw new ExecutorAuthorityError("executor authority response timestamps are out of order");
  }
  const htmlUrl = boundHtmlUrl(payload.html_url, input.executor.run_id, input.executor.run_attempt);
  return Object.freeze({
    request_path: requestPath,
    http_status: 200 as const,
    response: payload,
    run_id: input.executor.run_id,
    run_attempt: input.executor.run_attempt,
    html_url: htmlUrl,
    status: payload.status,
    conclusion,
    created_at: created,
    run_started_at: started,
    updated_at: updated,
  });
};

/**
 * Verifies the retiring attempt (completed with a real terminal conclusion)
 * and the replacement attempt (in_progress with null conclusion) for exactly
 * the two recorded endpoints. Fail closed on every transport, status,
 * malformed/oversize, timestamp, or identity violation. A completed
 * historical executor can never be reassigned as the replacement.
 *
 * The injected clock is validated before any network read, sampled again
 * after both response reads, must never regress, and is the sole authority
 * for the future-evidence check and the observed_at instant.
 */
export const verifyExecutorHandoverAuthority = async (
  input: Readonly<{
    token: string;
    fetcher: typeof fetch;
    retiring: SentinelProviderExecutorV1;
    next: SentinelProviderExecutorV1;
    now: () => number;
  }>,
): Promise<VerifiedExecutorAuthority> => {
  // Validate the initial clock before any request so a NaN/Infinity/invalid
  // clock can never trigger a network read.
  const initialIso = normalizeClock(input.now(), "initial");
  const initialMs = Date.parse(initialIso);
  const retiring = await verifyAttempt({
    token: input.token,
    fetcher: input.fetcher,
    executor: input.retiring,
  });
  const next = await verifyAttempt({
    token: input.token,
    fetcher: input.fetcher,
    executor: input.next,
  });
  if (retiring.status !== "completed") {
    throw new ExecutorAuthorityError("retiring executor attempt is not completed");
  }
  if (!(TERMINAL_CONCLUSIONS as readonly string[]).includes(retiring.conclusion ?? "")) {
    throw new ExecutorAuthorityError("retiring executor conclusion is not a real terminal conclusion");
  }
  if (next.status !== "in_progress") {
    throw new ExecutorAuthorityError("replacement executor attempt is not in progress");
  }
  if (next.conclusion !== null) {
    throw new ExecutorAuthorityError("replacement executor attempt has a conclusion");
  }
  // Final clock after both reads: a legitimate updated_at observed during the
  // reads must not be rejected, regression must fail closed, and the final
  // instant is authoritative for the future check and observed_at.
  const finalIso = normalizeClock(input.now(), "final");
  const finalMs = Date.parse(finalIso);
  if (finalMs < initialMs) {
    throw new ExecutorAuthorityError("executor authority clock regressed");
  }
  for (const attempt of [retiring, next]) {
    if (Date.parse(attempt.updated_at) > finalMs) {
      throw new ExecutorAuthorityError("executor authority response contains evidence from the future");
    }
  }
  return Object.freeze({
    observed_at: finalIso,
    retiring: Object.freeze({
      ...retiring,
      status: "completed" as const,
      conclusion: retiring.conclusion as SentinelProviderStopConclusion,
    }),
    next: Object.freeze({
      ...next,
      status: "in_progress" as const,
      conclusion: null,
    }),
  });
};

/**
 * Verifies the single current executor attempt used to authorize a protected
 * operation. The supplied candidate is first reduced through the strict local
 * parser (exactly the approved repository/workflow with positive safe run
 * id/attempt), then read through the same exact run-attempt endpoint with the
 * same strict transport/identity/body rules as the handover verifier. Only an
 * in_progress status with a null conclusion authorizes; a completed, failed,
 * stale, foreign, or malformed attempt fails closed with a safe
 * ExecutorAuthorityError.
 *
 * The injected clock is validated before any network read, sampled again after
 * the response read, must never regress, and is the sole authority for the
 * future-evidence check and the observed_at instant.
 */
export const verifyActiveProviderExecutorAuthority = async (
  input: Readonly<{
    token: string;
    fetcher: typeof fetch;
    executor: SentinelProviderExecutorV1;
    now: () => number;
  }>,
): Promise<VerifiedActiveExecutorAuthority> => {
  // Validate the initial clock before any request so a NaN/Infinity/invalid
  // clock can never trigger a network read.
  const initialIso = normalizeClock(input.now(), "initial");
  const initialMs = Date.parse(initialIso);
  const parsed = parseProviderExecutorIdentity(input.executor);
  if (parsed === null) throw new ExecutorAuthorityError("executor workflow identity is not approved");
  const attempt = await verifyAttempt({
    token: input.token,
    fetcher: input.fetcher,
    executor: parsed,
  });
  if (attempt.status !== "in_progress") {
    throw new ExecutorAuthorityError("executor attempt is not in progress");
  }
  if (attempt.conclusion !== null) {
    throw new ExecutorAuthorityError("executor attempt has a conclusion");
  }
  // Final clock after the read: a legitimate updated_at observed during the
  // read must not be rejected, regression must fail closed, and the final
  // instant is authoritative for the future check and observed_at.
  const finalIso = normalizeClock(input.now(), "final");
  const finalMs = Date.parse(finalIso);
  if (finalMs < initialMs) {
    throw new ExecutorAuthorityError("executor authority clock regressed");
  }
  if (Date.parse(attempt.updated_at) > finalMs) {
    throw new ExecutorAuthorityError("executor authority response contains evidence from the future");
  }
  return Object.freeze({
    observed_at: finalIso,
    attempt: Object.freeze({
      ...attempt,
      status: "in_progress" as const,
      conclusion: null,
    }),
  });
};
