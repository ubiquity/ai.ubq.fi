import { runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import { getKv } from "./kv.ts";
import { MAX_ACCEPTED_JSON_BODY_BYTES, observeRawBodyOnce } from "./request.ts";
import {
  canonicalSentinelUpstreamJson,
  emptySentinelUpstreamTrace,
  parseSentinelUpstreamTrace,
  SENTINEL_UPSTREAM_MAX_BYTES,
  SENTINEL_UPSTREAM_MAX_CHUNKS,
  type SentinelUpstreamRecorder,
  type SentinelUpstreamTrace,
} from "./sentinel_upstream_capture.ts";
import {
  bindSentinelIncidentIndexEvidence,
  completeSentinelIncidentFailureEvent,
  createSentinelIncidentFailureEventFromEnvironment,
  isSentinelIncidentCaptureReference,
  isSentinelIncidentId,
  isSentinelIncidentIndexRow,
  readySentinelIncidentFailureEvent,
  recordSentinelIncidentIndexObservation,
  SENTINEL_INCIDENT_CAPTURE_REF_PREFIX,
  SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS,
  SENTINEL_INCIDENT_INDEX_PREFIX,
  SENTINEL_INCIDENT_TTL_MS,
  type SentinelIncidentCaptureReference,
  type SentinelIncidentFailureEvent,
  sentinelIncidentFingerprint,
  type SentinelIncidentIndexRow,
} from "./sentinel_incident_outbox.ts";
import { base64UrlDecode, base64UrlEncode, encodeHex, isRecord } from "./utils.ts";

export const SENTINEL_REPLAY_TTL_MS = 48 * 60 * 60 * 1_000;
/**
 * Status rows are bounded non-sensitive metadata (request id, status, reason,
 * fingerprint, expiry). They outlive their payload by one additional replay TTL
 * so a request whose encrypted evidence has expired can still be reported as
 * `expired` instead of decaying into `unknown` at exactly the same instant.
 */
export const SENTINEL_REPLAY_STATUS_TTL_MS = 2 * SENTINEL_REPLAY_TTL_MS;
export const SENTINEL_REPLAY_CHUNK_BYTES = 48 * 1_024;
export const SENTINEL_REPLAY_MAX_BODY_BYTES = MAX_ACCEPTED_JSON_BODY_BYTES;
export const SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES = 1 * 1_024 * 1_024;
/** Bounded observed downstream terminal/error body retained with a failure. */
export const SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES = 64 * 1_024;
export const SENTINEL_REPLAY_EXPORT_PAGE_LIMIT = 1;
export const SENTINEL_REPLAY_MANIFEST_PREFIX = ["uos_ai", "sentinel_replay", "v1", "manifest"] as const;
export const SENTINEL_REPLAY_DEDUPE_PREFIX = ["uos_ai", "sentinel_replay", "v1", "dedupe"] as const;
export const SENTINEL_REPLAY_CHUNK_PREFIX = ["uos_ai", "sentinel_replay", "v1", "chunk"] as const;
/** Per-request capture status/correlation row, written with the capture. */
export const SENTINEL_REPLAY_REQUEST_PREFIX = ["uos_ai", "sentinel_replay", "v1", "request"] as const;

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();
const AES_GCM_IV_BYTES = 12;
const REPLAY_KEY_BYTES = 32;
const ENVELOPE_VERSION = 1;
/** Private plaintext metadata cut over to version 2 with required upstream evidence. */
const REPLAY_PLAINTEXT_VERSION_V2 = 2;
/** Current private plaintext: adds request settings, coverage and downstream outcome. */
const REPLAY_PLAINTEXT_VERSION = 3;
/** v2 fingerprint frame namespace; the outer crypto transport stays v1. */
const FINGERPRINT_NAMESPACE_V2 = "uos-sentinel-replay-v2:fingerprint";
const CASE_GROUP_NAMESPACE_V1 = "uos-sentinel-replay-v1:case-group";
/**
 * The private metadata carries the sealed upstream trace as per-chunk base64,
 * so its bound is derived from the bounded capture rather than fixed: base64
 * expansion of the full accepted upstream capture, per-chunk base64 padding and
 * JSON framing, the retained downstream body (carried in both of its metadata
 * projections), and the remaining bounded metadata. Encode and decode share
 * this one bound, so a capture the recorder accepted stays exportable.
 */
const MAX_REPLAY_UPSTREAM_METADATA_BYTES = Math.ceil(SENTINEL_UPSTREAM_MAX_BYTES / 3) * 4 + SENTINEL_UPSTREAM_MAX_CHUNKS * 8;
/** Base64 expansion of the retained downstream body carried in both metadata projections. */
const MAX_REPLAY_DOWNSTREAM_METADATA_BYTES = 2 * Math.ceil(SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES / 3) * 4;
/** Remaining bounded metadata; the entire metadata envelope previously fit in this allowance. */
const MAX_REPLAY_AUXILIARY_METADATA_BYTES = 256 * 1_024;
const MAX_REPLAY_METADATA_BYTES = MAX_REPLAY_UPSTREAM_METADATA_BYTES + MAX_REPLAY_DOWNSTREAM_METADATA_BYTES + MAX_REPLAY_AUXILIARY_METADATA_BYTES;
const MAX_REPLAY_PLAINTEXT_BYTES = SENTINEL_REPLAY_MAX_BODY_BYTES + MAX_REPLAY_METADATA_BYTES + 4;
const MAX_REPLAY_CIPHERTEXT_BYTES = MAX_REPLAY_PLAINTEXT_BYTES + 1_024 * 1_024 + 16;
const MAX_REPLAY_CHUNKS = Math.ceil(MAX_REPLAY_CIPHERTEXT_BYTES / SENTINEL_REPLAY_CHUNK_BYTES);
const MAX_CAPTURE_ID_CHARS = 128;
const MAX_SSE_EVENT_CHARS = 16 * 1_024 * 1_024;
const HEX_DIGEST = /^[0-9a-f]{64}$/;
const CAPTURE_ID = /^[A-Za-z0-9_-]+$/;
const KV_CURSOR = /^[A-Za-z0-9_-]+={0,2}$/;
const KEY_DERIVATION_SALT = TEXT_ENCODER.encode("uos-sentinel-replay-v1");

const COMPATIBILITY_HEADER_NAMES = [
  "accept",
  "openai-beta",
  "openai-organization",
  "openai-project",
  "originator",
  "user-agent",
  "x-codex-client-version",
  "x-stainless-arch",
  "x-stainless-lang",
  "x-stainless-os",
  "x-stainless-package-version",
  "x-stainless-retry-count",
  "x-stainless-runtime",
  "x-stainless-runtime-version",
] as const;
const COMPATIBILITY_HEADER_NAME_SET = new Set<string>(COMPATIBILITY_HEADER_NAMES);

export type SentinelCompatibilityHeaders = Readonly<Record<string, string>>;

export type SentinelFailureObservation = Readonly<{
  status: number;
  stream: boolean | null;
  completed: boolean;
  terminal_type: string | null;
  failure_kind: string | null;
  synthetic_terminal_type: string | null;
  provider_route: string;
}>;

export type SentinelClientFailureObservation = Readonly<{
  status: number;
  stream: boolean;
  completed: boolean;
  terminal_type: string | null;
  failure_kind: string | null;
  framing_valid: boolean;
  provider_route: string;
  /** Client-visible error code when the observed downstream body carried one. */
  error_code: string | null;
  /** Validation discriminator (`param`) when the observed body carried one. */
  error_param: string | null;
  /** Bounded base64 of the observed downstream terminal/error body, or null. */
  terminal_body_base64: string | null;
  /** Whether that retained body was cut at the fixed bound. */
  terminal_body_truncated: boolean;
}>;

export type SentinelClientBodyObservation = Readonly<{
  stream: boolean;
  completed: boolean;
  terminal_type: string | null;
  failure_kind: string | null;
  framing_valid: boolean;
  error_code?: string | null;
  error_param?: string | null;
  terminal_body_base64?: string | null;
  terminal_body_truncated?: boolean;
}>;

export type AcceptedSentinelReplayInput = Readonly<{
  endpoint: string;
  method: string;
  body: Uint8Array<ArrayBuffer>;
  content_type: string | null;
  compatibility_headers: SentinelCompatibilityHeaders;
  request_id: string;
  git_sha: string;
  deno_revision: string;
  /** Sealed immutable v2 upstream trace; absent means request-only evidence. */
  upstream?: SentinelUpstreamTrace;
  /** Request-owned passive upstream recorder carried until the snapshot handoff. */
  upstreamRecorder?: SentinelUpstreamRecorder;
}>;

export type SentinelReplayCaptureCandidate = {
  readonly endpoint: string;
  readonly method: string;
  body: Uint8Array<ArrayBuffer> | null;
  readonly content_type: string | null;
  readonly compatibility_headers: SentinelCompatibilityHeaders;
  readonly request_id: string;
  readonly git_sha: string;
  readonly deno_revision: string;
  /** Why no accepted body was captured, when that happened. */
  body_omitted_reason: SentinelReplayBodyOmissionReason | null;
};

/** Why a body the gateway accepted was not carried into a capture. */
export type SentinelReplayBodyOmissionReason = "body_over_limit" | "body_unavailable" | "non_post";

/**
 * Why a request produced no encrypted payload at all. In addition to the body
 * omissions, an authenticated request rejected before capture setup (quota
 * admission) still publishes an explicit status row rather than looking like a
 * request that never happened.
 */
export type SentinelReplayCaptureOmissionReason = SentinelReplayBodyOmissionReason | "rejected_before_capture";

/** Why a stored capture cannot be replayed in full. */
export type SentinelReplayUnavailableReason =
  | "request_body_omitted"
  | "upstream_trace_empty"
  | "upstream_trace_truncated"
  | "upstream_attempt_pending"
  | "upstream_timing_unavailable"
  | "upstream_response_headers_unavailable"
  | "downstream_terminal_body_unavailable"
  | "observed_serving_model_unavailable"
  | "provider_state_not_reproducible";

export const SENTINEL_REPLAY_UNAVAILABLE_REASONS: readonly SentinelReplayUnavailableReason[] = Object.freeze([
  "request_body_omitted",
  "upstream_trace_empty",
  "upstream_trace_truncated",
  "upstream_attempt_pending",
  "upstream_timing_unavailable",
  "upstream_response_headers_unavailable",
  "downstream_terminal_body_unavailable",
  "observed_serving_model_unavailable",
  "provider_state_not_reproducible",
]);

/** Request-declared settings read back from the recorded request bytes. */
export type SentinelReplaySettings = Readonly<{
  source: "recorded_request_body" | "unavailable";
  provider_route: string;
  model_requested: string | null;
  reasoning_requested: string | null;
  stream_requested: boolean | null;
  stream_observed: boolean | null;
}>;

/** Observed downstream terminal/error outcome carried with the capture. */
export type SentinelReplayDownstream = Readonly<{
  terminal_type: string | null;
  failure_kind: string | null;
  error_code: string | null;
  error_param: string | null;
  body_base64: string | null;
  body_truncated: boolean;
}>;

export type SentinelReplayPlaintext = Readonly<{
  /** 2 = request/upstream only; 3 = adds settings, coverage and downstream outcome. */
  version: 2 | 3;
  captured_at_ms: number;
  endpoint: string;
  method: string;
  content_type: string | null;
  compatibility_headers: SentinelCompatibilityHeaders;
  failure_signature: string;
  observation: SentinelFailureObservation;
  client_observation: SentinelClientFailureObservation;
  request_id: string;
  git_sha: string;
  deno_revision: string;
  upstream: SentinelUpstreamTrace;
  /** Version-3 additions; absent on a version-2 capture. */
  settings?: SentinelReplaySettings;
  capture_status?: "ready" | "incomplete";
  replay_coverage?: "full" | "partial" | "unavailable";
  unavailable?: readonly SentinelReplayUnavailableReason[];
  body_sha256?: string;
  body_bytes?: number;
  downstream?: SentinelReplayDownstream;
  body: Uint8Array<ArrayBuffer>;
}>;

export type SentinelReplayManifest = Readonly<{
  version: 1;
  capture_id: string;
  fingerprint: string;
  case_group_digest: string;
  captured_at_ms: number;
  expires_at_ms: number;
  algorithm: "AES-256-GCM";
  compression: "gzip";
  iv: string;
  chunk_count: number;
  ciphertext_bytes: number;
}>;

export type ExportedSentinelReplayCapture = Readonly<{
  manifest: SentinelReplayManifest;
  chunks: readonly string[];
}>;

/**
 * Per-request capture status. It exists so a failure is discoverable by its
 * request id even when nothing was stored: a missing replay key or a failed
 * persist must never look like an empty replay history.
 */
export type SentinelReplayCaptureStatus = "ready" | "incomplete" | "disabled" | "failed" | "expired" | "unknown";

export type SentinelReplayCaptureStatusRow = Readonly<{
  version: 1;
  request_id: string;
  status: SentinelReplayCaptureStatus;
  reason: string | null;
  captured_at_ms: number;
  manifest_key: Deno.KvKey | null;
  fingerprint: string | null;
  expires_at_ms: number | null;
}>;

const REQUEST_ID = /^[A-Za-z0-9._:-]{1,128}$/;
const STORED_CAPTURE_STATUSES = new Set<SentinelReplayCaptureStatus>(["ready", "incomplete", "disabled", "failed"]);
const STATUS_REASON = /^[A-Za-z0-9_.:-]{1,128}$/;

export const isSentinelReplayRequestId = (value: unknown): value is string => typeof value === "string" && REQUEST_ID.test(value);

export const isSentinelReplayCaptureStatusRow = (value: unknown): value is SentinelReplayCaptureStatusRow => {
  if (
    !isRecord(value) ||
    value.version !== 1 ||
    !isSentinelReplayRequestId(value.request_id) ||
    typeof value.status !== "string" ||
    !STORED_CAPTURE_STATUSES.has(value.status as SentinelReplayCaptureStatus) ||
    (value.reason !== null && (typeof value.reason !== "string" || !STATUS_REASON.test(value.reason))) ||
    typeof value.captured_at_ms !== "number" ||
    !Number.isSafeInteger(value.captured_at_ms) ||
    value.captured_at_ms < 0 ||
    (value.manifest_key !== null && (!Array.isArray(value.manifest_key) || value.manifest_key.length !== 7)) ||
    (value.fingerprint !== null && (typeof value.fingerprint !== "string" || !HEX_DIGEST.test(value.fingerprint))) ||
    (value.expires_at_ms !== null && (typeof value.expires_at_ms !== "number" || !Number.isSafeInteger(value.expires_at_ms)))
  )
    return false;
  if (value.status === "ready" || value.status === "incomplete") {
    return value.manifest_key !== null && value.fingerprint !== null && value.expires_at_ms !== null;
  }
  return value.manifest_key === null && value.expires_at_ms === null;
};

export type SentinelReplayPersistResult =
  | Readonly<{ status: "stored"; manifest: SentinelReplayManifest; manifest_key?: Deno.KvKey }>
  | Readonly<{ status: "duplicate"; fingerprint: string; manifest_key?: Deno.KvKey }>
  | Readonly<{ status: "disabled"; reason: "key_missing" | "kv_unavailable" }>;

type PersistDependencies = Readonly<{
  kv: Deno.Kv;
  keyBytes: Uint8Array<ArrayBuffer>;
  now?: () => number;
  randomUuid?: () => string;
  randomBytes?: (length: number) => Uint8Array<ArrayBuffer>;
  incidentEvent?: Deno.KvEntry<SentinelIncidentFailureEvent>;
}>;

const cloneBytes = (value: Uint8Array): Uint8Array<ArrayBuffer> => new Uint8Array(value);

const concatBytes = (parts: readonly Uint8Array[]): Uint8Array<ArrayBuffer> => {
  const length = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const output: Uint8Array<ArrayBuffer> = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
};

const stableHeaderText = (headers: SentinelCompatibilityHeaders): string =>
  Object.entries(headers)
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([name, value]) => `${name}:${value}`)
    .join("\n");

export const normalizeSentinelCompatibilityHeaders = (headers: Headers): SentinelCompatibilityHeaders => {
  const normalized: Record<string, string> = {};
  for (const name of COMPATIBILITY_HEADER_NAMES) {
    const value = headers.get(name)?.trim();
    if (value) normalized[name] = value;
  }
  return normalized;
};

/** A blank Content-Type header is treated as absent. */
const optionalContentType = (value: string | null): string | null => {
  const trimmed = value?.trim();
  return trimmed === undefined || trimmed === "" ? null : trimmed;
};

export const captureAcceptedSentinelReplayInput = (req: Request, requestId: string): SentinelReplayCaptureCandidate | null => {
  if (req.method !== "POST") return null;
  const url = new URL(req.url);
  const candidate: SentinelReplayCaptureCandidate = {
    endpoint: `${url.pathname}${url.search}`,
    method: req.method,
    body: null,
    content_type: optionalContentType(req.headers.get("content-type")),
    compatibility_headers: normalizeSentinelCompatibilityHeaders(req.headers),
    request_id: requestId,
    git_sha: runtimeGitSha(),
    deno_revision: runtimeDeploymentId(),
    body_omitted_reason: null,
  };
  observeRawBodyOnce(
    req,
    (bytes) => {
      candidate.body = bytes;
    },
    (reason) => {
      candidate.body_omitted_reason = reason;
    }
  );
  return candidate;
};

/**
 * Materialize the accepted body. A declined body is never silent: the
 * candidate keeps a bounded omission reason so a caller can publish a capture
 * status instead of an empty replay history.
 */
export const materializeSentinelReplayInput = (candidate: SentinelReplayCaptureCandidate | null): AcceptedSentinelReplayInput | null => {
  if (!candidate?.body) {
    if (candidate) candidate.body_omitted_reason ??= "body_unavailable";
    return null;
  }
  const body = candidate.body;
  candidate.body = null;
  return {
    endpoint: candidate.endpoint,
    method: candidate.method,
    body,
    content_type: candidate.content_type,
    compatibility_headers: candidate.compatibility_headers,
    request_id: candidate.request_id,
    git_sha: candidate.git_sha,
    deno_revision: candidate.deno_revision,
  };
};

export const zeroSentinelReplayInput = (input: AcceptedSentinelReplayInput | null | undefined): void => {
  input?.body.fill(0);
};

/**
 * Narrow application-terminal snapshot helper: copy the accepted body and
 * seal the request-owned upstream recorder together, before the original body
 * is zeroed or any await. HMAC, encryption and the plaintext metadata all use
 * this same immutable trace; the recorder is disposed so zeroing the original
 * never erases the snapshot and no later read retains additional bytes.
 */
export const snapshotSentinelReplayInput = (input: AcceptedSentinelReplayInput): AcceptedSentinelReplayInput => {
  const recorder = input.upstreamRecorder;
  const upstream = recorder ? recorder.snapshotAndSeal() : (input.upstream ?? emptySentinelUpstreamTrace());
  recorder?.dispose();
  return {
    endpoint: input.endpoint,
    method: input.method,
    body: cloneBytes(input.body),
    content_type: input.content_type,
    compatibility_headers: input.compatibility_headers,
    request_id: input.request_id,
    git_sha: input.git_sha,
    deno_revision: input.deno_revision,
    upstream,
  };
};

/** Discard a recorder that never reached a snapshot (zero retained bytes). */
export const disposeSentinelUpstreamRecorder = (input: AcceptedSentinelReplayInput | null | undefined): void => {
  input?.upstreamRecorder?.dispose();
};

export const discardSentinelReplayCaptureCandidate = (candidate: SentinelReplayCaptureCandidate | null | undefined): void => {
  candidate?.body?.fill(0);
  if (candidate) candidate.body = null;
};

export const sentinelFailureSignature = (observation: SentinelClientFailureObservation): string =>
  JSON.stringify({
    status: observation.status,
    stream: observation.stream,
    completed: observation.completed,
    terminal_type: observation.terminal_type,
    failure_kind: observation.failure_kind,
    framing_valid: observation.framing_valid,
    provider_route: observation.provider_route,
  });

const boundedFailureKind = (value: unknown): string | null => (typeof value === "string" && /^[A-Za-z0-9_.:-]{1,128}$/.test(value) ? value : null);

/** A client-visible validation discriminator: bounded, printable, non-secret. */
const boundedErrorParam = (value: unknown): string | null => (typeof value === "string" && /^[A-Za-z0-9_.:[\]-]{1,128}$/.test(value) ? value : null);

/** Bounded base64 of an observed downstream terminal body, with its truncation state. */
const boundedTerminalBody = (bytes: Uint8Array | null): Readonly<{ base64: string | null; truncated: boolean }> => {
  if (!bytes || bytes.byteLength === 0) return { base64: null, truncated: false };
  const bounded = bytes.subarray(0, SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES);
  let binary = "";
  for (const byte of bounded) binary += String.fromCharCode(byte);
  return { base64: btoa(binary), truncated: bounded.byteLength < bytes.byteLength };
};

const errorKind = (value: unknown): string | null => {
  if (!isRecord(value)) return null;
  return boundedFailureKind(value.code) ?? boundedFailureKind(value.type);
};

const errorParam = (value: unknown): string | null => (isRecord(value) ? boundedErrorParam(value.param) : null);

/** Semantic terminal of an embeddings-job payload, or null for other statuses. */
const embeddingsJobObservation = (
  semanticStatus: string | null,
  nestedError: string | null
): Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null => {
  if (semanticStatus === "failed") {
    return { completed: false, terminal_type: "job.failed", failure_kind: nestedError ?? "job_failed" };
  }
  if (semanticStatus === "queued" || semanticStatus === "running" || semanticStatus === "in_progress") {
    return { completed: false, terminal_type: "job.queued", failure_kind: nestedError };
  }
  if (semanticStatus === "succeeded" || semanticStatus === "completed") {
    return { completed: true, terminal_type: "job.succeeded", failure_kind: null };
  }
  return null;
};

const responseSemanticObservation = (
  status: number,
  parsed: Record<string, unknown>
): Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null => {
  const semanticStatus = typeof parsed.status === "string" ? parsed.status.trim().toLowerCase() : null;
  const object = typeof parsed.object === "string" ? parsed.object.trim().toLowerCase() : null;
  const nestedError = errorKind(parsed.error);
  if (object === "embeddings.job") {
    const job = embeddingsJobObservation(semanticStatus, nestedError);
    if (job !== null) return job;
  }
  if (semanticStatus === "failed") {
    return { completed: false, terminal_type: "response.failed", failure_kind: nestedError ?? "response_failed", error_code: nestedError };
  }
  if (semanticStatus === "incomplete") {
    const details = isRecord(parsed.incomplete_details) ? parsed.incomplete_details : null;
    return {
      completed: false,
      terminal_type: "response.incomplete",
      failure_kind: boundedFailureKind(details?.reason) ?? nestedError,
    };
  }
  if (semanticStatus === "completed" || semanticStatus === "succeeded") {
    return { completed: true, terminal_type: "response.completed", failure_kind: null };
  }
  if (isRecord(parsed.error)) {
    return {
      completed: false,
      terminal_type: "http.error",
      failure_kind: nestedError,
      error_code: nestedError,
      error_param: errorParam(parsed.error),
    };
  }
  if (status === 202) return { completed: false, terminal_type: "http.accepted", failure_kind: null };
  return null;
};

export const inspectSentinelBufferedResponse = (status: number, contentType: string, bytes: Uint8Array): SentinelClientBodyObservation => {
  let semantic: Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null = null;
  if (contentType.toLowerCase().includes("json")) {
    try {
      const parsed: unknown = JSON.parse(TEXT_DECODER.decode(bytes));
      if (isRecord(parsed) && !Array.isArray(parsed)) {
        semantic = responseSemanticObservation(status, parsed);
        // A non-2xx JSON body the semantic reader did not classify is still an
        // HTTP error: its own `error.code`/`error.param` are the client-visible
        // cause and must survive instead of being dropped as a bare status.
        if (semantic === null && status >= 400 && isRecord(parsed.error)) {
          semantic = {
            completed: false,
            terminal_type: "http.error",
            failure_kind: errorKind(parsed.error),
            error_code: errorKind(parsed.error),
            error_param: errorParam(parsed.error),
          };
        }
      }
    } catch {
      // HTTP status remains authoritative when a buffered body is not valid JSON.
    }
  }
  const withBody = (observation: SentinelClientBodyObservation): SentinelClientBodyObservation =>
    observation.completed
      ? observation
      : (() => {
          const body = boundedTerminalBody(bytes);
          return { ...observation, terminal_body_base64: body.base64, terminal_body_truncated: body.truncated };
        })();
  if (semantic) return withBody({ stream: false, framing_valid: true, ...semantic });
  if (status >= 400) {
    return withBody({
      stream: false,
      completed: false,
      terminal_type: "http.error",
      failure_kind: null,
      framing_valid: true,
      error_code: null,
      error_param: null,
    });
  }
  if (status === 202) {
    return {
      stream: false,
      completed: false,
      terminal_type: "http.accepted",
      failure_kind: null,
      framing_valid: true,
    };
  }
  return {
    stream: false,
    completed: true,
    terminal_type: "http.completed",
    failure_kind: null,
    framing_valid: true,
  };
};

const boundedContentLength = (headers: Headers): number | null => {
  const raw = headers.get("content-length");
  if (raw === null) return null;
  if (!/^(?:0|[1-9]\d*)$/.test(raw)) throw new Error("Response Content-Length is invalid");
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed)) throw new Error("Response Content-Length is invalid");
  return parsed;
};

const readBoundedResponseClone = async (response: Response): Promise<Uint8Array<ArrayBuffer>> => {
  const declared = boundedContentLength(response.headers);
  if (declared !== null && declared > SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES) {
    throw new Error("Response body is too large for sentinel inspection");
  }
  const clone = response.clone();
  if (!clone.body) return new Uint8Array();
  const reader = clone.body.getReader();
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength > SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES - total) {
        void reader.cancel().catch(() => {});
        throw new Error("Response body is too large for sentinel inspection");
      }
      total += value.byteLength;
      chunks.push(new Uint8Array(value));
    }
    return concatBytes(chunks);
  } finally {
    for (const chunk of chunks) chunk.fill(0);
    reader.releaseLock();
  }
};

export const inspectSentinelBufferedResponseBody = async (response: Response): Promise<SentinelClientBodyObservation | null> => {
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  try {
    bytes = await readBoundedResponseClone(response);
    return inspectSentinelBufferedResponse(response.status, response.headers.get("content-type")?.toLowerCase() ?? "", bytes);
  } catch {
    return null;
  } finally {
    bytes?.fill(0);
  }
};

const terminalRank = (terminalType: string | null): number => {
  if (terminalType === "response.failed" || terminalType === "error") return 3;
  if (terminalType === "response.incomplete") return 2;
  if (terminalType === "response.completed" || terminalType === "[DONE]") return 1;
  return 0;
};

const SSE_KNOWN_FIELDS = new Set(["event", "data", "id", "retry"]);

type SseEventFields = Readonly<{
  eventName: string | null;
  data: readonly string[];
  hasUnknownField: boolean;
}>;

/** Splits one raw SSE event into its `event` name, `data` lines and field check. */
const parseSseEventFields = (rawEvent: string): SseEventFields => {
  const data: string[] = [];
  let eventName: string | null = null;
  let hasUnknownField = false;
  for (const line of rawEvent.split("\n")) {
    if (!line || line.startsWith(":")) continue;
    const separator = line.indexOf(":");
    const field = separator < 0 ? line : line.slice(0, separator);
    const value = separator < 0 ? "" : line.slice(separator + 1).replace(/^ /, "");
    if (!SSE_KNOWN_FIELDS.has(field)) hasUnknownField = true;
    if (field === "event") eventName = value;
    else if (field === "data") data.push(value);
  }
  return { eventName, data, hasUnknownField };
};

/** The observed semantic fields of one SSE event, before stream/framing context. */
type SseEventSemantic = Omit<SentinelClientBodyObservation, "stream" | "framing_valid">;

/** The terminal observation for a failed Responses terminal event. */
const failedEventObservation = (type: string, response: Record<string, unknown> | null, topLevelError: string | null): SseEventSemantic => ({
  completed: false,
  terminal_type: type,
  failure_kind: errorKind(response?.error) ?? topLevelError ?? "response_failed",
  error_code: errorKind(response?.error) ?? topLevelError,
  error_param: errorParam(response?.error),
});

/** The terminal observation for an incomplete Responses terminal event. */
const incompleteEventObservation = (type: string, response: Record<string, unknown> | null, topLevelError: string | null): SseEventSemantic => {
  const details = response && isRecord(response.incomplete_details) ? response.incomplete_details : null;
  return {
    completed: false,
    terminal_type: type,
    failure_kind: boundedFailureKind(details?.reason) ?? errorKind(response?.error) ?? topLevelError,
    error_code: errorKind(response?.error) ?? topLevelError,
    error_param: errorParam(response?.error),
  };
};

/** The terminal observation for an error event or a top-level error payload. */
const errorEventObservation = (parsed: Record<string, unknown>, topLevelError: string | null): SseEventSemantic => {
  const nested = isRecord(parsed.error) ? parsed.error : parsed;
  return {
    completed: false,
    terminal_type: "error",
    failure_kind: topLevelError ?? "error",
    error_code: topLevelError ?? "error",
    error_param: errorParam(nested),
  };
};

/** The terminal observation one parsed SSE event carries, or null when it is not terminal. */
const sseTerminalObservation = (
  type: string | null,
  parsed: Record<string, unknown>,
  response: Record<string, unknown> | null,
  topLevelError: string | null
): SseEventSemantic | null => {
  if (type === "response.failed") return failedEventObservation(type, response, topLevelError);
  if (type === "response.incomplete") return incompleteEventObservation(type, response, topLevelError);
  if (type === "error" || isRecord(parsed.error)) return errorEventObservation(parsed, topLevelError);
  if (type === "response.completed") return { completed: true, terminal_type: type, failure_kind: null };
  return null;
};

const sseEventObservation = (rawEvent: string): SseEventSemantic | null => {
  const { eventName, data } = parseSseEventFields(rawEvent);
  if (!data.length) return null;
  const joined = data.join("\n");
  if (joined === "[DONE]") return { completed: true, terminal_type: "[DONE]", failure_kind: null };
  let parsed: unknown;
  try {
    parsed = JSON.parse(joined);
  } catch {
    return null;
  }
  if (!isRecord(parsed) || Array.isArray(parsed)) return null;
  const type = typeof parsed.type === "string" ? parsed.type : eventName;
  const response = isRecord(parsed.response) && !Array.isArray(parsed.response) ? parsed.response : null;
  const topLevelError = errorKind(parsed.error) ?? (type === "error" ? boundedFailureKind(parsed.code) : null);
  return sseTerminalObservation(type, parsed, response, topLevelError);
};

/** Failure kind used when no terminal SSE frame was observed. */
const missingTerminalFailureKind = (framingValid: boolean, terminalMissing: boolean, termination: "eof" | "read_error"): string | null => {
  if (framingValid) return null;
  if (!terminalMissing) return "invalid_sse_framing";
  return termination === "read_error" ? "stream_read_error" : "missing_sse_terminal";
};

export type SentinelSseInspector = Readonly<{
  push: (bytes: Uint8Array) => void;
  finish: (termination?: "eof" | "read_error") => SentinelClientBodyObservation;
}>;

export const createSentinelSseInspector = (): SentinelSseInspector => {
  const decoder = new TextDecoder();
  let buffer = "";
  let pendingCarriageReturn = false;
  let droppingOversizedEvent = false;
  let observedFrame = false;
  let framingValid = true;
  let terminal: Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null = null;
  let terminalBody: string | null = null;
  let terminalBodyTruncated = false;

  const observeEvent = (rawEvent: string): void => {
    const { data, hasUnknownField } = parseSseEventFields(rawEvent);
    if (hasUnknownField) framingValid = false;
    if (data.length && data.join("\n") !== "[DONE]") {
      try {
        const parsed: unknown = JSON.parse(data.join("\n"));
        if (!isRecord(parsed) || Array.isArray(parsed)) framingValid = false;
      } catch {
        framingValid = false;
      }
    }
    const observation = sseEventObservation(rawEvent);
    if (!observation) return;
    if (terminalRank(observation.terminal_type) > terminalRank(terminal?.terminal_type ?? null)) {
      terminal = observation;
      // Only a terminal event's own bytes are retained, bounded, so the
      // observed downstream terminal is replayable without keeping the trace.
      const retained = boundedTerminalBody(TEXT_ENCODER.encode(rawEvent));
      terminalBody = retained.base64;
      terminalBodyTruncated = retained.truncated;
    }
  };
  const process = (): void => {
    for (;;) {
      const boundary = buffer.indexOf("\n\n");
      if (boundary < 0) break;
      const rawEvent = buffer.slice(0, boundary);
      buffer = buffer.slice(boundary + 2);
      observedFrame = true;
      if (droppingOversizedEvent) {
        droppingOversizedEvent = false;
        continue;
      }
      observeEvent(rawEvent);
    }
    if (buffer.length > MAX_SSE_EVENT_CHARS) {
      framingValid = false;
      droppingOversizedEvent = true;
      buffer = "";
    }
  };
  const append = (text: string, final = false): void => {
    let normalized = pendingCarriageReturn ? `\r${text}` : text;
    pendingCarriageReturn = !final && normalized.endsWith("\r");
    if (pendingCarriageReturn) normalized = normalized.slice(0, -1);
    normalized = normalized.replaceAll("\r\n", "\n").replaceAll("\r", "\n");
    buffer += normalized;
    process();
  };

  return {
    push(bytes) {
      append(decoder.decode(bytes, { stream: true }));
    },
    finish(termination = "eof") {
      append(decoder.decode(), true);
      if (pendingCarriageReturn) {
        buffer += "\n";
        pendingCarriageReturn = false;
      }
      process();
      const terminalMissing = terminal === null;
      if (!observedFrame || buffer.trim() || droppingOversizedEvent || terminalMissing) framingValid = false;
      return {
        stream: true,
        completed: terminal?.completed ?? false,
        terminal_type: terminal?.terminal_type ?? null,
        failure_kind: terminal?.failure_kind ?? missingTerminalFailureKind(framingValid, terminalMissing, termination),
        framing_valid: framingValid,
        error_code: terminal?.error_code ?? null,
        error_param: terminal?.error_param ?? null,
        terminal_body_base64: terminalBody,
        terminal_body_truncated: terminalBodyTruncated,
      };
    },
  };
};

export const inspectSentinelSse = (bytes: Uint8Array): SentinelClientBodyObservation => {
  const inspector = createSentinelSseInspector();
  inspector.push(bytes);
  return inspector.finish();
};

/** Terminal type used when the internal observation carries no authoritative one. */
const fallbackTerminalType = (internal: SentinelFailureObservation, cancelled: boolean): string | null => {
  if (
    internal.terminal_type === "response.completed" ||
    internal.terminal_type === "response.failed" ||
    internal.terminal_type === "response.incomplete" ||
    internal.terminal_type === "error" ||
    cancelled
  ) {
    return internal.terminal_type;
  }
  if (internal.status >= 400) return "http.error";
  if (internal.completed) return "http.completed";
  return internal.terminal_type;
};

/**
 * The client-visible generic literal. It is a real observed value, but it is
 * never allowed to replace a trigger-specific transport cause (`read_error`,
 * `premature_eof`, `inactivity_timeout`, `malformed_event`, ...) in the
 * diagnostic `failure_kind` slot.
 */
const GENERIC_CLIENT_FAILURE_KINDS = new Set(["server_error", "error", "internal_error", "api_error"]);

const isGenericClientFailureKind = (value: string | null): boolean => value === null || GENERIC_CLIENT_FAILURE_KINDS.has(value);

/** Keep the more specific of the observed and internal causes; never the literal over a cause. */
const preferDiagnosticFailureKind = (observed: string | null, internal: string | null): string | null => {
  if (observed !== null && !isGenericClientFailureKind(observed)) return observed;
  if (internal !== null && !isGenericClientFailureKind(internal)) return internal;
  return observed ?? internal;
};

/**
 * A synthetic post-commit terminal is reported to the client as `server_error`,
 * but the transport cause that produced it must stay in the diagnostic slot.
 * The generic literal remains the fallback only when no more specific cause
 * exists at all.
 */
const diagnosticFailureKind = (
  cancelled: boolean,
  observedKind: string | null,
  internalKind: string | null,
  syntheticTerminal: string | null
): string | null => {
  if (cancelled) return null;
  const preferred = preferDiagnosticFailureKind(observedKind, internalKind);
  if (preferred !== null) return preferred;
  if (syntheticTerminal) return "server_error";
  return internalKind;
};

/** The client-visible error code, or the synthetic terminal literal when the client saw one. */
const clientErrorCode = (cancelled: boolean, body: SentinelClientBodyObservation | null | undefined, syntheticTerminal: string | null): string | null => {
  if (cancelled) return null;
  const observed = body?.error_code ?? null;
  if (observed !== null) return observed;
  return syntheticTerminal ? "server_error" : null;
};

export const resolveSentinelClientFailureObservation = (
  internal: SentinelFailureObservation,
  body?: SentinelClientBodyObservation | null
): SentinelClientFailureObservation => {
  const cancelled = internal.terminal_type === "cancelled";
  const syntheticTerminal = cancelled ? null : internal.synthetic_terminal_type;
  const fallbackTerminal = syntheticTerminal ?? fallbackTerminalType(internal, cancelled);
  const internalKind = cancelled ? null : internal.failure_kind;
  const observedKind = cancelled ? null : (body?.failure_kind ?? null);
  const bodyTerminalType = body ? body.terminal_type : fallbackTerminal;
  return {
    status: internal.status,
    stream: body ? body.stream : (internal.stream ?? false),
    completed: body ? body.completed : internal.completed,
    terminal_type: cancelled ? fallbackTerminal : bodyTerminalType,
    failure_kind: diagnosticFailureKind(cancelled, observedKind, internalKind, syntheticTerminal),
    framing_valid: body?.framing_valid ?? internal.stream !== true,
    provider_route: internal.provider_route,
    error_code: clientErrorCode(cancelled, body, syntheticTerminal),
    error_param: cancelled ? null : (body?.error_param ?? null),
    terminal_body_base64: cancelled ? null : (body?.terminal_body_base64 ?? null),
    terminal_body_truncated: cancelled ? false : (body?.terminal_body_truncated ?? false),
  };
};

const isGatewayOrProviderIncompleteReason = (value: string | null): boolean =>
  value !== null && /^(?:response_incomplete:)?(?:gateway|provider|upstream|server|network|timeout|deadline)[a-z0-9_.:-]*$/i.test(value);

const isPersistableSentinelFailure = (observation: SentinelFailureObservation | SentinelClientFailureObservation): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  if ("framing_valid" in observation && observation.stream && !observation.framing_valid) return true;
  if (observation.status < 400 && observation.completed && observation.terminal_type === "response.completed") {
    return false;
  }
  if (observation.status >= 400) return true;
  if (observation.terminal_type === "response.incomplete") {
    return (
      isGatewayOrProviderIncompleteReason(observation.failure_kind) ||
      ("synthetic_terminal_type" in observation && observation.synthetic_terminal_type !== null)
    );
  }
  if (observation.failure_kind !== null) return true;
  if ("synthetic_terminal_type" in observation && observation.synthetic_terminal_type !== null) return true;
  if (
    observation.terminal_type === "deadline" ||
    observation.terminal_type === "eof" ||
    observation.terminal_type === "error" ||
    observation.terminal_type === "response.failed"
  )
    return true;
  return observation.stream === true && !observation.completed && observation.terminal_type !== null && observation.terminal_type !== "cancelled";
};

export const shouldPersistSentinelReplay = (observation: SentinelFailureObservation, clientObservation?: SentinelClientFailureObservation): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  return isPersistableSentinelFailure(observation) || (clientObservation !== undefined && isPersistableSentinelFailure(clientObservation));
};

export const shouldSignalSentinelIncident = (observation: SentinelFailureObservation, clientObservation: SentinelClientFailureObservation): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  if (clientObservation.stream && !clientObservation.framing_valid) return true;
  if (observation.status < 400 && observation.completed && observation.terminal_type === "response.completed") {
    return false;
  }
  if (observation.status >= 500) return true;
  if (isGatewayOrProviderIncompleteReason(clientObservation.failure_kind)) return true;
  if (observation.terminal_type === "response.incomplete") {
    return (
      isGatewayOrProviderIncompleteReason(observation.failure_kind) ||
      observation.synthetic_terminal_type !== null ||
      (clientObservation.terminal_type === "response.incomplete" && isGatewayOrProviderIncompleteReason(clientObservation.failure_kind))
    );
  }
  if (observation.synthetic_terminal_type !== null || observation.failure_kind !== null) return true;
  return observation.terminal_type === "deadline" || observation.terminal_type === "eof" || observation.terminal_type === "error";
};

export const decodeSentinelReplayKey = (raw: string): Uint8Array<ArrayBuffer> | null => {
  if (!/^[A-Za-z0-9_-]{43}=?$/.test(raw)) return null;
  try {
    const decoded = base64UrlDecode(raw);
    return decoded.byteLength === REPLAY_KEY_BYTES ? decoded : null;
  } catch {
    return null;
  }
};

const deriveKeyBytes = async (keyBytes: Uint8Array<ArrayBuffer>, purpose: "encryption" | "fingerprint" | "case-group"): Promise<ArrayBuffer> => {
  const material = await crypto.subtle.importKey("raw", keyBytes, "HKDF", false, ["deriveBits"]);
  return await crypto.subtle.deriveBits(
    {
      name: "HKDF",
      hash: "SHA-256",
      salt: KEY_DERIVATION_SALT,
      info: TEXT_ENCODER.encode(purpose),
    },
    material,
    256
  );
};

const importAesKey = async (keyBytes: Uint8Array<ArrayBuffer>): Promise<CryptoKey> =>
  await crypto.subtle.importKey("raw", await deriveKeyBytes(keyBytes, "encryption"), { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);

const importHmacKey = async (keyBytes: Uint8Array<ArrayBuffer>, purpose: "fingerprint" | "case-group"): Promise<CryptoKey> =>
  await crypto.subtle.importKey("raw", await deriveKeyBytes(keyBytes, purpose), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);

const hmacHex = async (keyBytes: Uint8Array<ArrayBuffer>, purpose: "fingerprint" | "case-group", parts: readonly Uint8Array[]): Promise<string> => {
  const key = await importHmacKey(keyBytes, purpose);
  const message = concatBytes(parts);
  try {
    const digest = await crypto.subtle.sign("HMAC", key, message);
    return encodeHex(new Uint8Array(digest));
  } finally {
    message.fill(0);
  }
};

const gzip = async (bytes: Uint8Array<ArrayBuffer>): Promise<Uint8Array<ArrayBuffer>> => {
  const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
};

const gunzip = async (bytes: Uint8Array<ArrayBuffer>): Promise<Uint8Array<ArrayBuffer>> => {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  const reader = stream.getReader();
  const parts: Uint8Array<ArrayBuffer>[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > MAX_REPLAY_PLAINTEXT_BYTES) throw new Error("Sentinel replay plaintext exceeds its size limit");
      parts.push(cloneBytes(value));
    }
  } finally {
    reader.releaseLock();
  }
  return concatBytes(parts);
};

type ReplayMetadata = Omit<SentinelReplayPlaintext, "body">;

const encodePlaintext = (metadata: ReplayMetadata, body: Uint8Array<ArrayBuffer>): Uint8Array<ArrayBuffer> => {
  const metadataBytes = TEXT_ENCODER.encode(JSON.stringify(metadata));
  if (metadataBytes.byteLength > MAX_REPLAY_METADATA_BYTES) throw new Error("Sentinel replay metadata is too large");
  if (body.byteLength > SENTINEL_REPLAY_MAX_BODY_BYTES) throw new Error("Sentinel replay body is too large");
  const size = new Uint8Array(4);
  new DataView(size.buffer).setUint32(0, metadataBytes.byteLength, false);
  return concatBytes([size, metadataBytes, body]);
};

const decodePlaintext = (bytes: Uint8Array<ArrayBuffer>): SentinelReplayPlaintext => {
  if (bytes.byteLength < 4 || bytes.byteLength > MAX_REPLAY_PLAINTEXT_BYTES) {
    throw new Error("Sentinel replay envelope size is invalid");
  }
  const metadataLength = new DataView(bytes.buffer, bytes.byteOffset, 4).getUint32(0, false);
  if (metadataLength > MAX_REPLAY_METADATA_BYTES) throw new Error("Sentinel replay metadata is too large");
  const bodyOffset = 4 + metadataLength;
  if (bodyOffset > bytes.byteLength) throw new Error("Sentinel replay metadata length is invalid");
  if (bytes.byteLength - bodyOffset > SENTINEL_REPLAY_MAX_BODY_BYTES) {
    throw new Error("Sentinel replay body is too large");
  }
  const parsed = JSON.parse(TEXT_DECODER.decode(bytes.subarray(4, bodyOffset)));
  if (!isReplayMetadata(parsed)) throw new Error("Sentinel replay metadata is invalid");
  return { ...parsed, body: cloneBytes(bytes.subarray(bodyOffset)) };
};

const isStringRecord = (value: unknown): value is Record<string, string> => isRecord(value) && Object.values(value).every((item) => typeof item === "string");

const isCompatibilityHeaders = (value: unknown): value is SentinelCompatibilityHeaders =>
  isStringRecord(value) &&
  Object.entries(value).every(([name, headerValue]) => COMPATIBILITY_HEADER_NAME_SET.has(name) && headerValue.length > 0 && headerValue.trim() === headerValue);

const isFailureObservation = (value: unknown): value is SentinelFailureObservation =>
  isRecord(value) &&
  typeof value.status === "number" &&
  Number.isSafeInteger(value.status) &&
  (value.stream === null || typeof value.stream === "boolean") &&
  typeof value.completed === "boolean" &&
  (value.terminal_type === null || typeof value.terminal_type === "string") &&
  (value.failure_kind === null || typeof value.failure_kind === "string") &&
  (value.synthetic_terminal_type === null || typeof value.synthetic_terminal_type === "string") &&
  typeof value.provider_route === "string";

const isClientFailureObservation = (value: unknown, requireRich: boolean): value is SentinelClientFailureObservation =>
  isRecord(value) &&
  typeof value.status === "number" &&
  Number.isSafeInteger(value.status) &&
  typeof value.stream === "boolean" &&
  typeof value.completed === "boolean" &&
  (value.terminal_type === null || typeof value.terminal_type === "string") &&
  (value.failure_kind === null || typeof value.failure_kind === "string") &&
  typeof value.framing_valid === "boolean" &&
  typeof value.provider_route === "string" &&
  (!requireRich ||
    ((value.error_code === null || boundedFailureKind(value.error_code) !== null) &&
      (value.error_param === null || boundedErrorParam(value.error_param) !== null) &&
      (value.terminal_body_base64 === null || typeof value.terminal_body_base64 === "string") &&
      typeof value.terminal_body_truncated === "boolean"));

const isReplaySettings = (value: unknown): value is SentinelReplaySettings =>
  isRecord(value) &&
  (value.source === "recorded_request_body" || value.source === "unavailable") &&
  typeof value.provider_route === "string" &&
  (value.model_requested === null || boundedFailureKind(value.model_requested) !== null) &&
  (value.reasoning_requested === null || boundedFailureKind(value.reasoning_requested) !== null) &&
  (value.stream_requested === null || typeof value.stream_requested === "boolean") &&
  (value.stream_observed === null || typeof value.stream_observed === "boolean");

const isReplayDownstream = (value: unknown): value is SentinelReplayDownstream =>
  isRecord(value) &&
  (value.terminal_type === null || typeof value.terminal_type === "string") &&
  (value.failure_kind === null || typeof value.failure_kind === "string") &&
  (value.error_code === null || boundedFailureKind(value.error_code) !== null) &&
  (value.error_param === null || boundedErrorParam(value.error_param) !== null) &&
  (value.body_base64 === null || typeof value.body_base64 === "string") &&
  typeof value.body_truncated === "boolean";

const isUnavailableReasons = (value: unknown): value is readonly SentinelReplayUnavailableReason[] =>
  Array.isArray(value) &&
  value.length <= SENTINEL_REPLAY_UNAVAILABLE_REASONS.length &&
  value.every((item) => typeof item === "string" && (SENTINEL_REPLAY_UNAVAILABLE_REASONS as readonly string[]).includes(item));

const REPLAY_METADATA_V2_KEYS = [
  "version",
  "captured_at_ms",
  "endpoint",
  "method",
  "content_type",
  "compatibility_headers",
  "failure_signature",
  "observation",
  "client_observation",
  "request_id",
  "git_sha",
  "deno_revision",
  "upstream",
] as const;

const REPLAY_METADATA_V3_KEYS = [
  ...REPLAY_METADATA_V2_KEYS,
  "settings",
  "capture_status",
  "replay_coverage",
  "unavailable",
  "body_sha256",
  "body_bytes",
  "downstream",
] as const;

const REPLAY_CAPTURE_STATUS_VALUES: readonly string[] = ["ready", "incomplete"];
const REPLAY_COVERAGE_VALUES: readonly string[] = ["full", "partial", "unavailable"];

const isReplayPlaintextVersion = (value: unknown): value is 2 | 3 => value === REPLAY_PLAINTEXT_VERSION_V2 || value === REPLAY_PLAINTEXT_VERSION;

const isOneOfLiterals = (value: unknown, allowed: readonly string[]): boolean => typeof value === "string" && allowed.includes(value);

/** The version-3 additions: settings, coverage and the downstream outcome. */
const isReplayMetadataV3 = (value: Record<string, unknown>): boolean => {
  if (!isReplaySettings(value.settings) || !isReplayDownstream(value.downstream)) return false;
  if (!isOneOfLiterals(value.capture_status, REPLAY_CAPTURE_STATUS_VALUES)) return false;
  if (!isOneOfLiterals(value.replay_coverage, REPLAY_COVERAGE_VALUES)) return false;
  if (!isUnavailableReasons(value.unavailable)) return false;
  if (typeof value.body_sha256 !== "string" || !HEX_DIGEST.test(value.body_sha256)) return false;
  if (typeof value.body_bytes !== "number" || !Number.isSafeInteger(value.body_bytes) || value.body_bytes < 0) return false;
  return true;
};

/** The metadata fields every plaintext version shares. */
const isReplayMetadataCommon = (value: Record<string, unknown>, version: 2 | 3): boolean => {
  if (!Number.isSafeInteger(value.captured_at_ms) || (value.captured_at_ms as number) < 0) return false;
  if (typeof value.endpoint !== "string" || typeof value.method !== "string") return false;
  if (value.content_type !== null && typeof value.content_type !== "string") return false;
  if (!isCompatibilityHeaders(value.compatibility_headers)) return false;
  if (typeof value.failure_signature !== "string") return false;
  if (!isFailureObservation(value.observation) || !isClientFailureObservation(value.client_observation, version === REPLAY_PLAINTEXT_VERSION)) {
    return false;
  }
  if (typeof value.request_id !== "string" || typeof value.git_sha !== "string") return false;
  if (typeof value.deno_revision !== "string") return false;
  return true;
};

/** The envelope key set must match the declared plaintext version exactly. */
const hasExactReplayMetadataKeys = (value: Record<string, unknown>, version: 2 | 3): boolean => {
  const expectedKeys: readonly string[] = version === REPLAY_PLAINTEXT_VERSION ? REPLAY_METADATA_V3_KEYS : REPLAY_METADATA_V2_KEYS;
  const actualKeys = Object.keys(value);
  return actualKeys.length === expectedKeys.length && actualKeys.every((key) => expectedKeys.includes(key));
};

const isReplayMetadata = (value: unknown): value is SentinelReplayPlaintext => {
  if (!isRecord(value)) return false;
  const version = value.version;
  if (!isReplayPlaintextVersion(version)) return false;
  if (!hasExactReplayMetadataKeys(value, version)) return false;
  if (!isReplayMetadataCommon(value, version)) return false;
  try {
    parseSentinelUpstreamTrace(value.upstream);
  } catch {
    return false;
  }
  if (version === REPLAY_PLAINTEXT_VERSION && !isReplayMetadataV3(value)) return false;
  return true;
};

const randomBytes = (length: number): Uint8Array<ArrayBuffer> => crypto.getRandomValues(new Uint8Array(length));

/**
 * Request-declared settings read back from the exact recorded request bytes.
 * This never invents a serving decision: `provider_route` and
 * `stream_observed` are observations, and the model/reasoning/stream values are
 * labelled as the recorded request's own declared settings.
 */
const requestSettingsFromBody = (body: Uint8Array<ArrayBuffer>, providerRoute: string, streamObserved: boolean | null): SentinelReplaySettings => {
  const observed: SentinelReplaySettings = {
    source: "unavailable",
    provider_route: providerRoute,
    model_requested: null,
    reasoning_requested: null,
    stream_requested: null,
    stream_observed: streamObserved,
  };
  try {
    const parsed: unknown = JSON.parse(TEXT_DECODER.decode(body));
    if (!isRecord(parsed) || Array.isArray(parsed)) return observed;
    const reasoning = isRecord(parsed.reasoning) ? boundedFailureKind(parsed.reasoning.effort) : null;
    return {
      source: "recorded_request_body",
      provider_route: providerRoute,
      model_requested: boundedFailureKind(parsed.model),
      reasoning_requested: reasoning ?? boundedFailureKind(parsed.reasoning_effort),
      stream_requested: typeof parsed.stream === "boolean" ? parsed.stream : null,
      stream_observed: streamObserved,
    };
  } catch {
    return observed;
  }
};

const downstreamObservation = (observation: SentinelClientFailureObservation): SentinelReplayDownstream => ({
  terminal_type: observation.terminal_type,
  failure_kind: observation.failure_kind,
  error_code: observation.error_code,
  error_param: observation.error_param,
  body_base64: observation.terminal_body_base64,
  body_truncated: observation.terminal_body_truncated,
});

/**
 * A 4xx rejection the gateway produced without dispatching a provider is
 * decided by the recorded request bytes and the gateway's own validation, so
 * its empty attempt list is correct evidence rather than a missing attempt.
 */
const isGatewayReproducibleRejection = (clientObservation: SentinelClientFailureObservation): boolean =>
  clientObservation.status >= 400 && clientObservation.status < 500;

/** Every reason this capture is not a complete, fully replayable record. */
const replayUnavailableReasons = (upstream: SentinelUpstreamTrace, clientObservation: SentinelClientFailureObservation): SentinelReplayUnavailableReason[] => {
  const reasons: SentinelReplayUnavailableReason[] = [];
  if (upstream.attempts.length === 0 && !isGatewayReproducibleRejection(clientObservation)) reasons.push("upstream_trace_empty");
  if (upstream.attempts_truncated || upstream.bytes_truncated || upstream.chunks_truncated) reasons.push("upstream_trace_truncated");
  if (upstream.attempts.some((attempt) => attempt.terminal === "pending")) reasons.push("upstream_attempt_pending");
  if (upstream.attempts.some((attempt) => attempt.started_at_ms === undefined)) reasons.push("upstream_timing_unavailable");
  if (upstream.attempts.some((attempt) => attempt.headers === undefined)) reasons.push("upstream_response_headers_unavailable");
  if (upstream.attempts.some((attempt) => attempt.headers_truncated === true)) reasons.push("upstream_response_headers_unavailable");
  if (clientObservation.terminal_body_base64 === null) reasons.push("downstream_terminal_body_unavailable");
  // An unreported serving-model label is optional diagnostic metadata. It is
  // never evidence the capture is missing: the recorded provider response bytes
  // are what replay consumes, so an unknown label must not make byte-complete
  // evidence permanently partial.
  return reasons;
};

const replayCoverageFor = (
  upstream: SentinelUpstreamTrace,
  clientObservation: SentinelClientFailureObservation,
  unavailable: readonly SentinelReplayUnavailableReason[]
): "full" | "partial" | "unavailable" => {
  if (unavailable.includes("upstream_trace_truncated") || unavailable.includes("upstream_attempt_pending")) return "partial";
  if (upstream.attempts.length === 0) {
    // No provider was dispatched. Only a deterministic gateway-side rejection
    // can be reproduced without provider state.
    const status = clientObservation.status;
    return status >= 400 && status < 500 ? "full" : "unavailable";
  }
  return unavailable.length === 0 ? "full" : "partial";
};

const encryptionAdditionalData = (fingerprint: string): Uint8Array<ArrayBuffer> => TEXT_ENCODER.encode(`uos-sentinel-replay-v1\0${fingerprint}`);

const splitChunks = (bytes: Uint8Array<ArrayBuffer>): Uint8Array<ArrayBuffer>[] => {
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  for (let offset = 0; offset < bytes.byteLength; offset += SENTINEL_REPLAY_CHUNK_BYTES) {
    chunks.push(cloneBytes(bytes.subarray(offset, offset + SENTINEL_REPLAY_CHUNK_BYTES)));
  }
  return chunks.length ? chunks : [new Uint8Array()];
};

const fingerprintParts = (
  input: AcceptedSentinelReplayInput,
  failureSignature: string,
  purpose: "fingerprint" | "case-group",
  upstream?: SentinelUpstreamTrace
): Uint8Array<ArrayBuffer>[] => {
  const frame = (value: Uint8Array<ArrayBuffer>): Uint8Array<ArrayBuffer>[] => {
    const length = new Uint8Array(8);
    new DataView(length.buffer).setBigUint64(0, BigInt(value.byteLength), false);
    return [length, value];
  };
  const common = [
    ...frame(TEXT_ENCODER.encode(purpose === "fingerprint" ? FINGERPRINT_NAMESPACE_V2 : CASE_GROUP_NAMESPACE_V1)),
    ...frame(TEXT_ENCODER.encode(input.method)),
    ...frame(TEXT_ENCODER.encode(input.endpoint)),
    ...frame(TEXT_ENCODER.encode(stableHeaderText(input.compatibility_headers))),
  ];
  // Keep the case-group identity exactly the v1 request-only HMAC. Only the
  // fingerprint gains one appended frame of canonical upstream JSON, so
  // different partial/complete traces cannot suppress one another.
  return purpose === "fingerprint"
    ? [
        ...common,
        ...frame(input.body),
        ...frame(TEXT_ENCODER.encode(failureSignature)),
        ...frame(TEXT_ENCODER.encode(upstream ? canonicalSentinelUpstreamJson(upstream) : "")),
      ]
    : [...common, ...frame(input.body)];
};

const dedupeManifestKey = (value: unknown): Deno.KvKey | null => {
  if (!isRecord(value) || !Array.isArray(value.manifest_key) || value.manifest_key.length !== 7) return null;
  return value.manifest_key as Deno.KvKey;
};

const ciphertextDigest = async (bytes: Uint8Array<ArrayBuffer>): Promise<string> => encodeHex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));

/**
 * Read and validate the winning manifest/chunks of an existing capture, then
 * bind the same actual capture to the stable index row in one CAS transaction.
 * The digest is the SHA-256 of the decoded concatenated ciphertext bytes,
 * exactly the consumer capture digest; the original expiry is retained and
 * nothing pretends a duplicate created new evidence. The winning capture is
 * authenticated/decrypted with the key already held by the caller so the bound
 * revision and provenance timestamp are the EXACT originals carried by that
 * encrypted capture; plaintext is zeroed and never persisted.
 */
const bindWinnerIndexEvidence = async (
  kv: Deno.Kv,
  indexFingerprint: string,
  observedAtMs: number,
  referenceFingerprint: string,
  manifestKey: Deno.KvKey,
  keyBytes: Uint8Array<ArrayBuffer>
): Promise<void> => {
  const manifestEntry = await kv.get<SentinelReplayManifest>(manifestKey);
  if (
    !manifestEntry.value ||
    !isSentinelReplayManifest(manifestEntry.value) ||
    manifestEntry.value.fingerprint !== referenceFingerprint ||
    !manifestMatchesKey(manifestKey, manifestEntry.value)
  )
    throw new Error("Sentinel incident replay manifest is unavailable");
  const chunks = await getChunks(kv, manifestEntry.value);
  let plaintext: SentinelReplayPlaintext | null = null;
  try {
    const digest = await ciphertextDigest(concatBytes(chunks));
    plaintext = await decryptExportedSentinelReplay({ manifest: manifestEntry.value, chunks: chunks.map(base64UrlEncode) }, keyBytes);
    await bindSentinelIncidentIndexEvidence(kv, indexFingerprint, {
      observedAtMs,
      captureId: manifestEntry.value.capture_id,
      gitSha: /^[0-9a-f]{40}$/.test(plaintext.git_sha) ? plaintext.git_sha : null,
      referenceFingerprint,
      manifestKey,
      manifestVersionstamp: manifestEntry.versionstamp,
      capturedAtMs: manifestEntry.value.captured_at_ms,
      digest,
      expiresAtMs: manifestEntry.value.expires_at_ms,
    });
  } finally {
    plaintext?.body.fill(0);
    for (const chunk of chunks) chunk.fill(0);
  }
};

const completeReplayIncidentEvent = async (
  kv: Deno.Kv,
  event: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined,
  readyAtMs: number,
  capture: Readonly<{ status: "stored" | "duplicate"; fingerprint: string; manifestKey: Deno.KvKey }> | Readonly<{ status: "unavailable" }>
): Promise<void> => {
  if (!event) return;
  if (!(await completeSentinelIncidentFailureEvent(kv, event, readyAtMs, capture))) {
    throw new Error("Sentinel incident capture completion conflicted");
  }
};

const resolveIndexFingerprint = async (input: AcceptedSentinelReplayInput, clientObservation: SentinelClientFailureObservation): Promise<string | null> => {
  try {
    return await sentinelIncidentFingerprint({
      endpoint: input.endpoint,
      method: input.method,
      observation: clientObservation,
    });
  } catch {
    // The index is best-effort for direct callers: the environment producer
    // already recorded this observation before the key lookup.
    return null;
  }
};

/**
 * Settles a capture that already exists (or lost the CAS race): the durable
 * index row, when present, is bound to the winning capture, and the incident
 * outbox event is completed. Nothing pretends the duplicate created evidence.
 */
const completeDuplicateCapture = async (
  dependencies: PersistDependencies,
  duplicate: Readonly<{
    fingerprint: string;
    manifestKey: Deno.KvKey;
    indexKey: Deno.KvKey | null;
    indexFingerprint: string | null;
    requestId: string;
    captureStatus: "ready" | "incomplete";
    capturedAtMs: number;
    expiresAtMs: number;
  }>,
  now: number
): Promise<SentinelReplayPersistResult> => {
  if (duplicate.indexKey !== null && duplicate.indexFingerprint !== null) {
    const indexEntry = await dependencies.kv.get<SentinelIncidentIndexRow>(duplicate.indexKey);
    if (indexEntry.value !== null) {
      if (!isSentinelIncidentIndexRow(indexEntry.value)) {
        throw new Error("Sentinel incident index record is invalid");
      }
      await bindWinnerIndexEvidence(dependencies.kv, duplicate.indexFingerprint, now, duplicate.fingerprint, duplicate.manifestKey, dependencies.keyBytes);
    }
  }
  await completeReplayIncidentEvent(dependencies.kv, dependencies.incidentEvent, now, {
    status: "duplicate",
    fingerprint: duplicate.fingerprint,
    manifestKey: duplicate.manifestKey,
  });
  // The request still resolves to real evidence: keep its status row pointing
  // at the winning capture rather than leaving the request unaccounted for.
  await writeSentinelReplayCaptureStatus(
    dependencies.kv,
    captureStatusRow({
      requestId: duplicate.requestId,
      status: duplicate.captureStatus,
      reason: null,
      capturedAtMs: duplicate.capturedAtMs,
      manifestKey: duplicate.manifestKey,
      fingerprint: duplicate.fingerprint,
      expiresAtMs: duplicate.expiresAtMs,
    })
  ).catch(() => {});
  return { status: "duplicate", fingerprint: duplicate.fingerprint, manifest_key: duplicate.manifestKey };
};

/** Encrypts the replay plaintext envelope under the request key. */
const encryptReplayPlaintext = async (
  metadata: ReplayMetadata,
  bodySnapshot: Uint8Array<ArrayBuffer>,
  iv: Uint8Array<ArrayBuffer>,
  keyBytes: Uint8Array<ArrayBuffer>,
  fingerprint: string
): Promise<Uint8Array<ArrayBuffer>> => {
  const encodedPlaintext = encodePlaintext(metadata, bodySnapshot);
  let compressed: Uint8Array<ArrayBuffer>;
  try {
    compressed = await gzip(encodedPlaintext);
  } finally {
    encodedPlaintext.fill(0);
  }
  if (compressed.byteLength + 16 > MAX_REPLAY_CIPHERTEXT_BYTES) {
    compressed.fill(0);
    throw new Error("Sentinel replay compressed payload is too large");
  }
  try {
    return new Uint8Array(
      await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: encryptionAdditionalData(fingerprint) }, await importAesKey(keyBytes), compressed)
    );
  } finally {
    compressed.fill(0);
  }
};

const requestStatusKey = (requestId: string): Deno.KvKey => [...SENTINEL_REPLAY_REQUEST_PREFIX, requestId];

const captureStatusRow = (
  input: Readonly<{
    requestId: string;
    status: SentinelReplayCaptureStatus;
    reason: string | null;
    capturedAtMs: number;
    manifestKey: Deno.KvKey | null;
    fingerprint: string | null;
    expiresAtMs: number | null;
  }>
): SentinelReplayCaptureStatusRow => {
  const row: SentinelReplayCaptureStatusRow = {
    version: 1,
    request_id: input.requestId,
    status: input.status,
    reason: input.reason,
    captured_at_ms: input.capturedAtMs,
    manifest_key: input.manifestKey === null ? null : [...input.manifestKey],
    fingerprint: input.fingerprint,
    expires_at_ms: input.expiresAtMs,
  };
  if (!isSentinelReplayCaptureStatusRow(row)) throw new Error("Sentinel replay capture status is invalid");
  return row;
};

/** Best-effort per-request status write; never replaces the caller's outcome. */
export const writeSentinelReplayCaptureStatus = async (kv: Deno.Kv, row: SentinelReplayCaptureStatusRow): Promise<void> => {
  if (!isSentinelReplayCaptureStatusRow(row)) throw new Error("Sentinel replay capture status is invalid");
  // The row outlives its payload so `expired` stays reportable; this is bounded
  // non-sensitive status metadata, never captured request or response content.
  await kv.set(requestStatusKey(row.request_id), row, { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS });
};

/**
 * Publish the explicit per-request status for a request that produced no
 * encrypted payload: an accepted body the fixed cap could not carry, or an
 * authenticated request rejected before capture setup. Best effort by design —
 * the caller's response or exception is never replaced by a status failure.
 */
export const recordSentinelReplayOmissionFromEnvironment = async (
  requestId: string,
  reason: SentinelReplayCaptureOmissionReason,
  nowMs: number = Date.now()
): Promise<void> => {
  if (!isSentinelReplayRequestId(requestId)) return;
  try {
    const kv = await getKv();
    if (!kv) return;
    await writeSentinelReplayCaptureStatus(
      kv,
      captureStatusRow({
        requestId,
        status: "disabled",
        reason,
        capturedAtMs: nowMs,
        manifestKey: null,
        fingerprint: null,
        expiresAtMs: null,
      })
    );
  } catch {
    // Diagnostic only: a status row can never replace the real outcome.
  }
};

/** The dedupe/manifest/incident-event writes every store attempt starts from. */
const replayStoreOperation = (
  dependencies: PersistDependencies,
  dedupeKey: Deno.KvKey,
  manifestKey: Deno.KvKey,
  manifest: SentinelReplayManifest,
  fingerprint: string,
  now: number,
  status: Readonly<{ requestId: string; captureStatus: "ready" | "incomplete" }>
): Deno.AtomicOperation => {
  let operation = dependencies.kv
    .atomic()
    .check({ key: dedupeKey, versionstamp: null })
    .set(dedupeKey, { manifest_key: manifestKey }, { expireIn: SENTINEL_REPLAY_TTL_MS })
    .set(manifestKey, manifest, { expireIn: SENTINEL_REPLAY_TTL_MS })
    .set(
      requestStatusKey(status.requestId),
      captureStatusRow({
        requestId: status.requestId,
        status: status.captureStatus,
        reason: null,
        capturedAtMs: manifest.captured_at_ms,
        manifestKey,
        fingerprint,
        expiresAtMs: manifest.expires_at_ms,
      }),
      { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS }
    );
  if (dependencies.incidentEvent) {
    const readyEvent = readySentinelIncidentFailureEvent(dependencies.incidentEvent, now, {
      status: "stored",
      fingerprint,
      manifestKey,
    });
    operation = operation
      .check({ key: dependencies.incidentEvent.key, versionstamp: dependencies.incidentEvent.versionstamp })
      .set(dependencies.incidentEvent.key, readyEvent, { expireIn: SENTINEL_INCIDENT_TTL_MS });
  }
  return operation;
};

/** Adds the evidence row and capture reference for an existing index observation. */
const withIncidentIndexEvidence = (
  operation: Deno.AtomicOperation,
  indexRow: Readonly<{ key: Deno.KvKey; entry: Deno.KvEntry<SentinelIncidentIndexRow> }>,
  context: Readonly<{
    gitSha: string;
    captureId: string;
    evidenceDigest: string;
    manifestKey: Deno.KvKey;
    capturedAtMs: number;
    expiresAtMs: number;
    now: number;
    fingerprint: string;
  }>
): Deno.AtomicOperation => {
  if (!isSentinelIncidentIndexRow(indexRow.entry.value)) {
    throw new Error("Sentinel incident index record is invalid");
  }
  const next: SentinelIncidentIndexRow = {
    ...indexRow.entry.value,
    failing_revision: /^[0-9a-f]{40}$/.test(context.gitSha) ? context.gitSha : null,
    provenance: { ...indexRow.entry.value.provenance, captured_at_ms: context.capturedAtMs },
    evidence_ref: { ref: `capture:${context.captureId}`, digest: context.evidenceDigest },
    evidence_expires_at_ms: context.expiresAtMs,
  };
  if (!isSentinelIncidentIndexRow(next)) throw new Error("Sentinel incident index record is invalid");
  const reference: SentinelIncidentCaptureReference = { version: 1, manifest_key: [...context.manifestKey] };
  return operation
    .check({ key: indexRow.key, versionstamp: indexRow.entry.versionstamp })
    .set([...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, next.incident_id, context.fingerprint], reference, { expireIn: context.expiresAtMs - context.now })
    .set(indexRow.key, next);
};

/** Reads the durable index observation once, when one exists. */
const readIncidentIndexEntry = async (
  kv: Deno.Kv,
  indexKey: Deno.KvKey | null
): Promise<Readonly<{ key: Deno.KvKey; entry: Deno.KvEntry<SentinelIncidentIndexRow> }> | null> => {
  if (indexKey === null) return null;
  const entry = await kv.get<SentinelIncidentIndexRow>(indexKey);
  return entry.value === null ? null : { key: indexKey, entry };
};

/** Writes the chunks, then commits the envelope with a bounded CAS retry loop. */
const storeReplayEnvelope = async (
  context: Readonly<{
    dependencies: PersistDependencies;
    gitSha: string;
    chunks: readonly Uint8Array<ArrayBuffer>[];
    dedupeKey: Deno.KvKey;
    manifestKey: Deno.KvKey;
    manifest: SentinelReplayManifest;
    indexKey: Deno.KvKey | null;
    indexFingerprint: string | null;
    captureId: string;
    evidenceDigest: string;
    now: number;
    expiresAtMs: number;
    requestId: string;
    captureStatus: "ready" | "incomplete";
  }>
): Promise<SentinelReplayPersistResult> => {
  const { dependencies, chunks, dedupeKey, manifestKey, manifest, indexKey, indexFingerprint, captureId, evidenceDigest, now, expiresAtMs } = context;
  const fingerprint = manifest.fingerprint;
  const cleanupChunks = async (): Promise<void> => {
    await Promise.all(chunks.map((_chunk, index) => dependencies.kv.delete([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index])));
  };
  try {
    await Promise.all(
      chunks.map((chunk, index) =>
        dependencies.kv.set([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index], chunk, {
          expireIn: SENTINEL_REPLAY_TTL_MS,
        })
      )
    );
    let committed: Deno.KvCommitResult | Deno.KvCommitError | null = null;
    for (let attempt = 0; attempt < SENTINEL_INCIDENT_INDEX_MAX_CAS_ATTEMPTS; attempt += 1) {
      const indexRow = await readIncidentIndexEntry(dependencies.kv, indexKey);
      let operation = replayStoreOperation(dependencies, dedupeKey, manifestKey, manifest, fingerprint, now, {
        requestId: context.requestId,
        captureStatus: context.captureStatus,
      });
      if (indexRow !== null) {
        operation = withIncidentIndexEvidence(operation, indexRow, {
          gitSha: context.gitSha,
          captureId,
          evidenceDigest,
          manifestKey,
          capturedAtMs: manifest.captured_at_ms,
          expiresAtMs,
          now,
          fingerprint,
        });
      }
      committed = await operation.commit();
      if (committed.ok) return { status: "stored", manifest, manifest_key: manifestKey };
    }
    await cleanupChunks().catch(() => {});
    const winningDedupe = await dependencies.kv.get(dedupeKey);
    const winningManifestKey = dedupeManifestKey(winningDedupe.value);
    if (!winningManifestKey) throw new Error("Sentinel replay dedupe winner is unavailable");
    return await completeDuplicateCapture(
      dependencies,
      {
        fingerprint,
        manifestKey: winningManifestKey,
        indexKey,
        indexFingerprint,
        requestId: context.requestId,
        captureStatus: context.captureStatus,
        capturedAtMs: manifest.captured_at_ms,
        expiresAtMs,
      },
      now
    );
  } catch (error) {
    await cleanupChunks().catch(() => {});
    throw error;
  }
};

export const persistEncryptedSentinelReplay = async (
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  dependencies: PersistDependencies,
  clientObservation: SentinelClientFailureObservation = resolveSentinelClientFailureObservation(observation)
): Promise<SentinelReplayPersistResult> => {
  if (!shouldPersistSentinelReplay(observation, clientObservation)) {
    throw new Error("A successful request cannot be persisted as a sentinel replay");
  }
  if (dependencies.keyBytes.byteLength !== REPLAY_KEY_BYTES) throw new Error("Sentinel replay key must be 32 bytes");
  if (!isCompatibilityHeaders(input.compatibility_headers)) {
    throw new Error("Sentinel replay compatibility headers contain a disallowed value");
  }

  // Cancellation cleanup can zero the request-owned buffer while KV and
  // cryptographic operations are pending. One synchronous snapshot must feed
  // the digests and encrypted envelope so a capture cannot disagree with its
  // own manifest. Request-only callers emit the required empty upstream trace
  // with all truncation flags false; it is never labeled captured coverage.
  const upstreamTrace = input.upstream !== undefined ? parseSentinelUpstreamTrace(input.upstream) : emptySentinelUpstreamTrace();
  const bodySnapshot = cloneBytes(input.body);
  const snapshotInput: AcceptedSentinelReplayInput = { ...input, body: bodySnapshot };
  try {
    const now = dependencies.now?.() ?? Date.now();
    const failureSignature = sentinelFailureSignature(clientObservation);
    const fingerprint = await hmacHex(dependencies.keyBytes, "fingerprint", fingerprintParts(snapshotInput, failureSignature, "fingerprint", upstreamTrace));
    const caseGroupDigest = await hmacHex(dependencies.keyBytes, "case-group", fingerprintParts(snapshotInput, failureSignature, "case-group"));
    const unavailable = replayUnavailableReasons(upstreamTrace, clientObservation);
    const dedupeKey = [...SENTINEL_REPLAY_DEDUPE_PREFIX, fingerprint] as const;
    const indexFingerprint = await resolveIndexFingerprint(input, clientObservation);
    const indexKey: Deno.KvKey | null = indexFingerprint === null ? null : [...SENTINEL_INCIDENT_INDEX_PREFIX, indexFingerprint];
    const existingDedupe = await dependencies.kv.get(dedupeKey);
    if (existingDedupe.value !== null) {
      const manifestKey = dedupeManifestKey(existingDedupe.value);
      if (!manifestKey) throw new Error("Sentinel replay dedupe record is invalid");
      return await completeDuplicateCapture(
        dependencies,
        {
          fingerprint,
          manifestKey,
          indexKey,
          indexFingerprint,
          requestId: input.request_id,
          captureStatus: unavailable.length === 0 ? "ready" : "incomplete",
          capturedAtMs: now,
          expiresAtMs: now + SENTINEL_REPLAY_TTL_MS,
        },
        now
      );
    }

    const captureId = dependencies.randomUuid?.() ?? crypto.randomUUID();
    const iv = dependencies.randomBytes?.(AES_GCM_IV_BYTES) ?? randomBytes(AES_GCM_IV_BYTES);
    if (iv.byteLength !== AES_GCM_IV_BYTES) throw new Error("Sentinel replay IV must be 12 bytes");
    const metadata: ReplayMetadata = {
      version: REPLAY_PLAINTEXT_VERSION,
      captured_at_ms: now,
      endpoint: input.endpoint,
      method: input.method,
      content_type: input.content_type,
      compatibility_headers: input.compatibility_headers,
      failure_signature: failureSignature,
      observation,
      client_observation: clientObservation,
      request_id: input.request_id,
      git_sha: input.git_sha,
      deno_revision: input.deno_revision,
      upstream: upstreamTrace,
      settings: requestSettingsFromBody(bodySnapshot, observation.provider_route, observation.stream),
      capture_status: unavailable.length === 0 ? "ready" : "incomplete",
      replay_coverage: replayCoverageFor(upstreamTrace, clientObservation, unavailable),
      unavailable,
      body_sha256: await ciphertextDigest(bodySnapshot),
      body_bytes: bodySnapshot.byteLength,
      downstream: downstreamObservation(clientObservation),
    };
    const encrypted = await encryptReplayPlaintext(metadata, bodySnapshot, iv, dependencies.keyBytes, fingerprint);
    try {
      const chunks = splitChunks(encrypted);
      const expiresAtMs = now + SENTINEL_REPLAY_TTL_MS;
      const manifest: SentinelReplayManifest = {
        version: ENVELOPE_VERSION,
        capture_id: captureId,
        fingerprint,
        case_group_digest: caseGroupDigest,
        captured_at_ms: now,
        expires_at_ms: expiresAtMs,
        algorithm: "AES-256-GCM",
        compression: "gzip",
        iv: base64UrlEncode(iv),
        chunk_count: chunks.length,
        ciphertext_bytes: encrypted.byteLength,
      };

      const manifestKey = [...SENTINEL_REPLAY_MANIFEST_PREFIX, now, fingerprint, captureId] as const;
      const evidenceDigest = await ciphertextDigest(encrypted);
      try {
        return await storeReplayEnvelope({
          dependencies,
          gitSha: input.git_sha,
          chunks,
          dedupeKey,
          manifestKey,
          manifest,
          indexKey,
          indexFingerprint,
          captureId,
          evidenceDigest,
          now,
          expiresAtMs,
          requestId: input.request_id,
          captureStatus: unavailable.length === 0 ? "ready" : "incomplete",
        });
      } finally {
        for (const chunk of chunks) chunk.fill(0);
      }
    } finally {
      encrypted.fill(0);
    }
  } finally {
    bodySnapshot.fill(0);
  }
};

const readReplayKeyFromEnvironment = (): Uint8Array<ArrayBuffer> | null => {
  try {
    const raw = Deno.env.get("SENTINEL_REPLAY_KEY")?.trim();
    return raw ? decodeSentinelReplayKey(raw) : null;
  } catch {
    return null;
  }
};

export const persistSentinelReplayFromEnvironment = async (
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  clientObservation?: SentinelClientFailureObservation
): Promise<SentinelReplayPersistResult> => {
  let keyBytes: Uint8Array<ArrayBuffer> | null = null;
  let kv: Deno.Kv | null;
  let incidentEvent: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined;
  const resolvedClientObservation = clientObservation ?? resolveSentinelClientFailureObservation(observation);
  const now = Date.now();
  const recordStatus = async (status: "disabled" | "failed", reason: string, capturedAtMs: number): Promise<void> => {
    if (!kv || !isSentinelReplayRequestId(input.request_id)) return;
    try {
      await writeSentinelReplayCaptureStatus(
        kv,
        captureStatusRow({
          requestId: input.request_id,
          status,
          reason,
          capturedAtMs,
          manifestKey: null,
          fingerprint: null,
          expiresAtMs: null,
        })
      );
    } catch {
      // A status row is diagnostic: its own failure must never replace the real
      // persist outcome the caller already has.
    }
  };
  try {
    if (!shouldPersistSentinelReplay(observation, resolvedClientObservation)) {
      throw new Error("A successful request cannot be persisted as a sentinel replay");
    }
    kv = await getKv();
    if (!kv) return { status: "disabled", reason: "kv_unavailable" };
    if (shouldSignalSentinelIncident(observation, resolvedClientObservation)) {
      // The durable index observation is recorded BEFORE the key lookup and
      // encryption: even a missing key still leaves a discoverable incident
      // row with evidence_ref null. Unlike the transient event helper this
      // passive path is never environment-gated (it must work in the real
      // handler tests without a production deployment flag).
      try {
        await recordSentinelIncidentIndexObservation(kv, {
          endpoint: input.endpoint,
          method: input.method,
          gitSha: input.git_sha,
          observedAtMs: now,
          observation: resolvedClientObservation,
          classification: {
            provider: resolvedClientObservation.provider_route,
            model: null,
            reasoning: null,
            failure_kind: resolvedClientObservation.failure_kind,
          },
        });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "index_write_failed" }));
      }
      try {
        incidentEvent = (await createSentinelIncidentFailureEventFromEnvironment(kv, now)) ?? undefined;
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "outbox_write_failed" }));
      }
    }
    keyBytes = readReplayKeyFromEnvironment();
    if (!keyBytes) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, now, { status: "unavailable" });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }));
      }
      // A missing key must be visible on its request, never a silently empty
      // replay history.
      await recordStatus("disabled", "key_missing", now);
      return { status: "disabled", reason: "key_missing" };
    }
    try {
      return await persistEncryptedSentinelReplay(input, observation, { kv, keyBytes, now: () => now, incidentEvent }, resolvedClientObservation);
    } catch (error) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, Date.now(), { status: "unavailable" });
      } catch {
        console.warn("[ai.ubq.fi] sentinel_incident", JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }));
      }
      await recordStatus("failed", "persist_failed", now);
      throw error;
    }
  } finally {
    keyBytes?.fill(0);
    zeroSentinelReplayInput(input);
  }
};

const decodedIvIsValid = (value: string): boolean => {
  if (value.length < 16 || value.length > 24 || !/^[A-Za-z0-9_-]+$/.test(value)) return false;
  try {
    return base64UrlDecode(value).byteLength === AES_GCM_IV_BYTES;
  } catch {
    return false;
  }
};

export const isSentinelReplayManifest = (value: unknown): value is SentinelReplayManifest => {
  if (
    !isRecord(value) ||
    value.version !== ENVELOPE_VERSION ||
    typeof value.capture_id !== "string" ||
    value.capture_id.length < 1 ||
    value.capture_id.length > MAX_CAPTURE_ID_CHARS ||
    !CAPTURE_ID.test(value.capture_id) ||
    typeof value.fingerprint !== "string" ||
    !HEX_DIGEST.test(value.fingerprint) ||
    typeof value.case_group_digest !== "string" ||
    !HEX_DIGEST.test(value.case_group_digest) ||
    typeof value.captured_at_ms !== "number" ||
    !Number.isSafeInteger(value.captured_at_ms) ||
    value.captured_at_ms < 0 ||
    typeof value.expires_at_ms !== "number" ||
    !Number.isSafeInteger(value.expires_at_ms) ||
    value.expires_at_ms !== value.captured_at_ms + SENTINEL_REPLAY_TTL_MS ||
    value.algorithm !== "AES-256-GCM" ||
    value.compression !== "gzip" ||
    typeof value.iv !== "string" ||
    !decodedIvIsValid(value.iv) ||
    typeof value.chunk_count !== "number" ||
    !Number.isSafeInteger(value.chunk_count) ||
    value.chunk_count < 1 ||
    value.chunk_count > MAX_REPLAY_CHUNKS ||
    typeof value.ciphertext_bytes !== "number" ||
    !Number.isSafeInteger(value.ciphertext_bytes) ||
    value.ciphertext_bytes < 16 ||
    value.ciphertext_bytes > MAX_REPLAY_CIPHERTEXT_BYTES
  )
    return false;
  const minimumBytes = (value.chunk_count - 1) * SENTINEL_REPLAY_CHUNK_BYTES + 1;
  return value.ciphertext_bytes >= minimumBytes && value.ciphertext_bytes <= value.chunk_count * SENTINEL_REPLAY_CHUNK_BYTES;
};

const expectedChunkBytes = (manifest: SentinelReplayManifest, index: number): number =>
  index < manifest.chunk_count - 1 ? SENTINEL_REPLAY_CHUNK_BYTES : manifest.ciphertext_bytes - (manifest.chunk_count - 1) * SENTINEL_REPLAY_CHUNK_BYTES;

const assertChunkSize = (manifest: SentinelReplayManifest, index: number, bytes: Uint8Array): void => {
  if (bytes.byteLength !== expectedChunkBytes(manifest, index)) {
    throw new Error("Sentinel replay chunk size does not match its manifest");
  }
};

export const isExportedSentinelReplayCapture = (value: unknown): value is ExportedSentinelReplayCapture => {
  if (!isRecord(value) || !isSentinelReplayManifest(value.manifest) || !Array.isArray(value.chunks)) return false;
  if (value.chunks.length !== value.manifest.chunk_count) return false;
  try {
    for (let index = 0; index < value.chunks.length; index++) {
      const encoded = value.chunks[index];
      if (
        typeof encoded !== "string" ||
        encoded.length < 1 ||
        encoded.length > Math.ceil(SENTINEL_REPLAY_CHUNK_BYTES / 3) * 4 ||
        !/^[A-Za-z0-9_-]+$/.test(encoded)
      )
        return false;
      assertChunkSize(value.manifest, index, base64UrlDecode(encoded));
    }
    return true;
  } catch {
    return false;
  }
};

const getChunks = async (kv: Deno.Kv, manifest: SentinelReplayManifest): Promise<Uint8Array<ArrayBuffer>[]> => {
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  for (let offset = 0; offset < manifest.chunk_count; offset += 10) {
    const keys = Array.from(
      { length: Math.min(10, manifest.chunk_count - offset) },
      (_, index) => [...SENTINEL_REPLAY_CHUNK_PREFIX, manifest.capture_id, offset + index] as Deno.KvKey
    );
    const entries = await kv.getMany<readonly Uint8Array[]>(keys);
    for (let entryIndex = 0; entryIndex < entries.length; entryIndex++) {
      const entry = entries[entryIndex];
      if (!(entry.value instanceof Uint8Array)) throw new Error("Sentinel replay chunk is missing");
      assertChunkSize(manifest, offset + entryIndex, entry.value);
      chunks.push(cloneBytes(entry.value));
    }
  }
  return chunks;
};

const manifestEntryMatchesKey = (entry: Deno.KvEntry<SentinelReplayManifest>): boolean => {
  if (!SENTINEL_REPLAY_MANIFEST_PREFIX.every((part, index) => entry.key[index] === part)) return false;
  const suffix = entry.key.slice(SENTINEL_REPLAY_MANIFEST_PREFIX.length);
  return suffix.length === 3 && suffix[0] === entry.value.captured_at_ms && suffix[1] === entry.value.fingerprint && suffix[2] === entry.value.capture_id;
};

const manifestMatchesKey = (key: Deno.KvKey, manifest: SentinelReplayManifest): boolean => {
  const suffix = key.slice(SENTINEL_REPLAY_MANIFEST_PREFIX.length);
  return (
    SENTINEL_REPLAY_MANIFEST_PREFIX.every((part, index) => key[index] === part) &&
    suffix.length === 3 &&
    suffix[0] === manifest.captured_at_ms &&
    suffix[1] === manifest.fingerprint &&
    suffix[2] === manifest.capture_id
  );
};

export const listEncryptedSentinelReplays = async (
  kv: Deno.Kv,
  options: Readonly<{ afterMs: number; beforeMs: number; cursor?: string; limit?: number }>
): Promise<Readonly<{ captures: ExportedSentinelReplayCapture[]; cursor: string }>> => {
  if (!Number.isSafeInteger(options.afterMs) || options.afterMs < 0) {
    throw new Error("Sentinel replay export start is invalid");
  }
  if (!Number.isSafeInteger(options.beforeMs) || options.beforeMs < options.afterMs || options.beforeMs >= Number.MAX_SAFE_INTEGER) {
    throw new Error("Sentinel replay export end is invalid");
  }
  if (options.limit !== undefined && options.limit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
    throw new Error("Sentinel replay export limit must be one");
  }
  if (options.cursor !== undefined && (options.cursor.length < 1 || options.cursor.length > 2_048 || !KV_CURSOR.test(options.cursor))) {
    throw new Error("Sentinel replay export cursor is invalid");
  }
  const iterator = kv.list<SentinelReplayManifest>(
    {
      prefix: SENTINEL_REPLAY_MANIFEST_PREFIX,
      start: [...SENTINEL_REPLAY_MANIFEST_PREFIX, options.afterMs],
    },
    { cursor: options.cursor, limit: SENTINEL_REPLAY_EXPORT_PAGE_LIMIT }
  );
  const captures: ExportedSentinelReplayCapture[] = [];
  let rangeExhausted = false;
  for await (const entry of iterator) {
    if (!isSentinelReplayManifest(entry.value) || !manifestEntryMatchesKey(entry)) {
      throw new Error("Sentinel replay manifest is invalid");
    }
    if (entry.value.captured_at_ms < options.afterMs) throw new Error("Sentinel replay manifest order is invalid");
    if (entry.value.captured_at_ms > options.beforeMs) {
      rangeExhausted = true;
      break;
    }
    const chunks = await getChunks(kv, entry.value);
    captures.push({ manifest: entry.value, chunks: chunks.map(base64UrlEncode) });
    break;
  }
  return { captures, cursor: rangeExhausted ? "" : iterator.cursor };
};

export const listEncryptedSentinelIncidentReplays = async (
  kv: Deno.Kv,
  options: Readonly<{ incidentId: string; cursor?: string; limit?: number }>
): Promise<Readonly<{ captures: ExportedSentinelReplayCapture[]; cursor: string }>> => {
  if (!isSentinelIncidentId(options.incidentId)) throw new Error("Sentinel incident ID is invalid");
  if (options.limit !== undefined && options.limit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
    throw new Error("Sentinel replay export limit must be one");
  }
  if (options.cursor !== undefined && (options.cursor.length < 1 || options.cursor.length > 2_048 || !KV_CURSOR.test(options.cursor)))
    throw new Error("Sentinel replay export cursor is invalid");
  const prefix = [...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, options.incidentId] as const;
  const iterator = kv.list({ prefix }, { cursor: options.cursor, limit: SENTINEL_REPLAY_EXPORT_PAGE_LIMIT });
  const captures: ExportedSentinelReplayCapture[] = [];
  for await (const entry of iterator) {
    const fingerprint = entry.key.at(-1);
    if (
      entry.key.length !== prefix.length + 1 ||
      typeof fingerprint !== "string" ||
      !/^[0-9a-f]{64}$/.test(fingerprint) ||
      !isSentinelIncidentCaptureReference(entry.value)
    )
      throw new Error("Sentinel incident replay reference is invalid");
    const manifestEntry = await kv.get<SentinelReplayManifest>(entry.value.manifest_key);
    if (
      !manifestEntry.value ||
      !isSentinelReplayManifest(manifestEntry.value) ||
      manifestEntry.value.fingerprint !== fingerprint ||
      !manifestMatchesKey(entry.value.manifest_key, manifestEntry.value)
    )
      throw new Error("Sentinel incident replay manifest is unavailable");
    const chunks = await getChunks(kv, manifestEntry.value);
    captures.push({ manifest: manifestEntry.value, chunks: chunks.map(base64UrlEncode) });
    break;
  }
  return { captures, cursor: iterator.cursor };
};

/**
 * Read the capture status for one request id. `expired` is derived at read
 * time from the stored expiry; a request with no row is `unknown`, never a
 * silent empty history.
 */
export const readSentinelReplayCaptureStatus = async (kv: Deno.Kv, requestId: string, nowMs: number = Date.now()): Promise<SentinelReplayCaptureStatusRow> => {
  if (!isSentinelReplayRequestId(requestId)) throw new Error("Sentinel replay request ID is invalid");
  const entry = await kv.get<SentinelReplayCaptureStatusRow>(requestStatusKey(requestId));
  if (entry.value === null) {
    return {
      version: 1,
      request_id: requestId,
      status: "unknown",
      reason: "no_capture_record",
      captured_at_ms: nowMs,
      manifest_key: null,
      fingerprint: null,
      expires_at_ms: null,
    };
  }
  if (!isSentinelReplayCaptureStatusRow(entry.value)) throw new Error("Sentinel replay capture status record is invalid");
  if (entry.value.expires_at_ms !== null && entry.value.expires_at_ms <= nowMs) {
    return { ...entry.value, status: "expired" };
  }
  return entry.value;
};

/** Export the capture a request id points at, if its manifest is still present. */
export const listEncryptedSentinelReplaysByRequestId = async (
  kv: Deno.Kv,
  requestId: string
): Promise<Readonly<{ captures: ExportedSentinelReplayCapture[]; status: SentinelReplayCaptureStatusRow }>> => {
  const status = await readSentinelReplayCaptureStatus(kv, requestId);
  if (status.manifest_key === null || status.fingerprint === null) return { captures: [], status };
  const manifestEntry = await kv.get<SentinelReplayManifest>(status.manifest_key);
  if (
    !manifestEntry.value ||
    !isSentinelReplayManifest(manifestEntry.value) ||
    manifestEntry.value.fingerprint !== status.fingerprint ||
    !manifestMatchesKey(status.manifest_key, manifestEntry.value)
  ) {
    return { captures: [], status: { ...status, status: "expired", reason: "manifest_unavailable" } };
  }
  const chunks = await getChunks(kv, manifestEntry.value);
  return { captures: [{ manifest: manifestEntry.value, chunks: chunks.map(base64UrlEncode) }], status };
};

export const decryptExportedSentinelReplay = async (
  exported: ExportedSentinelReplayCapture,
  keyBytes: Uint8Array<ArrayBuffer>
): Promise<SentinelReplayPlaintext> => {
  if (keyBytes.byteLength !== REPLAY_KEY_BYTES) throw new Error("Sentinel replay key must be 32 bytes");
  if (!isExportedSentinelReplayCapture(exported)) throw new Error("Sentinel replay export is invalid");
  const decodedChunks = exported.chunks.map(base64UrlDecode);
  const ciphertext = concatBytes(decodedChunks);
  if (ciphertext.byteLength !== exported.manifest.ciphertext_bytes) {
    ciphertext.fill(0);
    for (const chunk of decodedChunks) chunk.fill(0);
    throw new Error("Sentinel replay ciphertext length does not match its manifest");
  }
  const iv = base64UrlDecode(exported.manifest.iv);
  let compressed: Uint8Array<ArrayBuffer>;
  try {
    compressed = new Uint8Array(
      await crypto.subtle.decrypt(
        { name: "AES-GCM", iv, additionalData: encryptionAdditionalData(exported.manifest.fingerprint) },
        await importAesKey(keyBytes),
        ciphertext
      )
    );
  } finally {
    ciphertext.fill(0);
    for (const chunk of decodedChunks) chunk.fill(0);
  }
  let decodedPlaintext: Uint8Array<ArrayBuffer>;
  try {
    decodedPlaintext = await gunzip(compressed);
  } finally {
    compressed.fill(0);
  }
  let plaintext: SentinelReplayPlaintext;
  try {
    plaintext = decodePlaintext(decodedPlaintext);
  } finally {
    decodedPlaintext.fill(0);
  }
  const accepted: AcceptedSentinelReplayInput = {
    endpoint: plaintext.endpoint,
    method: plaintext.method,
    body: plaintext.body,
    content_type: plaintext.content_type,
    compatibility_headers: plaintext.compatibility_headers,
    request_id: plaintext.request_id,
    git_sha: plaintext.git_sha,
    deno_revision: plaintext.deno_revision,
  };
  try {
    const fingerprint = await hmacHex(keyBytes, "fingerprint", fingerprintParts(accepted, plaintext.failure_signature, "fingerprint", plaintext.upstream));
    const caseGroupDigest = await hmacHex(keyBytes, "case-group", fingerprintParts(accepted, plaintext.failure_signature, "case-group"));
    if (
      fingerprint !== exported.manifest.fingerprint ||
      caseGroupDigest !== exported.manifest.case_group_digest ||
      plaintext.captured_at_ms !== exported.manifest.captured_at_ms ||
      sentinelFailureSignature(plaintext.client_observation) !== plaintext.failure_signature
    ) {
      throw new Error("Sentinel replay manifest integrity check failed");
    }
    return plaintext;
  } catch (error) {
    plaintext.body.fill(0);
    throw error;
  }
};
