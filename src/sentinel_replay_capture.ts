import { runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import { getKv } from "./kv.ts";
import { MAX_ACCEPTED_JSON_BODY_BYTES, observeRawBodyOnce } from "./request.ts";
import {
  completeSentinelIncidentFailureEvent,
  createSentinelIncidentFailureEventFromEnvironment,
  isSentinelIncidentCaptureReference,
  isSentinelIncidentId,
  readySentinelIncidentFailureEvent,
  SENTINEL_INCIDENT_CAPTURE_REF_PREFIX,
  SENTINEL_INCIDENT_TTL_MS,
  type SentinelIncidentFailureEvent,
} from "./sentinel_incident_outbox.ts";
import { base64UrlDecode, base64UrlEncode, encodeHex, isRecord } from "./utils.ts";

export const SENTINEL_REPLAY_TTL_MS = 48 * 60 * 60 * 1_000;
export const SENTINEL_REPLAY_CHUNK_BYTES = 48 * 1_024;
export const SENTINEL_REPLAY_MAX_BODY_BYTES = MAX_ACCEPTED_JSON_BODY_BYTES;
export const SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES = 1 * 1_024 * 1_024;
export const SENTINEL_REPLAY_EXPORT_PAGE_LIMIT = 1;
export const SENTINEL_REPLAY_MANIFEST_PREFIX = ["uos_ai", "sentinel_replay", "v1", "manifest"] as const;
export const SENTINEL_REPLAY_DEDUPE_PREFIX = ["uos_ai", "sentinel_replay", "v1", "dedupe"] as const;
export const SENTINEL_REPLAY_CHUNK_PREFIX = ["uos_ai", "sentinel_replay", "v1", "chunk"] as const;

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();
const AES_GCM_IV_BYTES = 12;
const REPLAY_KEY_BYTES = 32;
const ENVELOPE_VERSION = 1;
const MAX_REPLAY_METADATA_BYTES = 256 * 1_024;
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
}>;

export type SentinelClientBodyObservation = Readonly<{
  stream: boolean;
  completed: boolean;
  terminal_type: string | null;
  failure_kind: string | null;
  framing_valid: boolean;
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
};

export type SentinelReplayPlaintext = Readonly<{
  version: 1;
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

export const captureAcceptedSentinelReplayInput = (
  req: Request,
  requestId: string,
): SentinelReplayCaptureCandidate | null => {
  if (req.method !== "POST") return null;
  const url = new URL(req.url);
  const candidate: SentinelReplayCaptureCandidate = {
    endpoint: `${url.pathname}${url.search}`,
    method: req.method,
    body: null,
    content_type: req.headers.get("content-type")?.trim() || null,
    compatibility_headers: normalizeSentinelCompatibilityHeaders(req.headers),
    request_id: requestId,
    git_sha: runtimeGitSha(),
    deno_revision: runtimeDeploymentId(),
  };
  observeRawBodyOnce(req, (bytes) => {
    candidate.body = bytes;
  });
  return candidate;
};

export const materializeSentinelReplayInput = (
  candidate: SentinelReplayCaptureCandidate | null,
): AcceptedSentinelReplayInput | null => {
  if (!candidate?.body) return null;
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

export const discardSentinelReplayCaptureCandidate = (
  candidate: SentinelReplayCaptureCandidate | null | undefined,
): void => {
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

const boundedFailureKind = (value: unknown): string | null =>
  typeof value === "string" && /^[A-Za-z0-9_.:-]{1,128}$/.test(value) ? value : null;

const errorKind = (value: unknown): string | null => {
  if (!isRecord(value)) return null;
  return boundedFailureKind(value.code) ?? boundedFailureKind(value.type);
};

const responseSemanticObservation = (
  status: number,
  parsed: Record<string, unknown>,
): Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null => {
  const semanticStatus = typeof parsed.status === "string" ? parsed.status.trim().toLowerCase() : null;
  const object = typeof parsed.object === "string" ? parsed.object.trim().toLowerCase() : null;
  const nestedError = errorKind(parsed.error);
  if (object === "embeddings.job") {
    if (semanticStatus === "failed") {
      return { completed: false, terminal_type: "job.failed", failure_kind: nestedError ?? "job_failed" };
    }
    if (semanticStatus === "queued" || semanticStatus === "running" || semanticStatus === "in_progress") {
      return { completed: false, terminal_type: "job.queued", failure_kind: nestedError };
    }
    if (semanticStatus === "succeeded" || semanticStatus === "completed") {
      return { completed: true, terminal_type: "job.succeeded", failure_kind: null };
    }
  }
  if (semanticStatus === "failed") {
    return { completed: false, terminal_type: "response.failed", failure_kind: nestedError ?? "response_failed" };
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
    return { completed: false, terminal_type: "http.error", failure_kind: nestedError };
  }
  if (status === 202) return { completed: false, terminal_type: "http.accepted", failure_kind: null };
  return null;
};

export const inspectSentinelBufferedResponse = (
  status: number,
  contentType: string,
  bytes: Uint8Array,
): SentinelClientBodyObservation => {
  let semantic: Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null = null;
  if (contentType.toLowerCase().includes("json")) {
    try {
      const parsed: unknown = JSON.parse(TEXT_DECODER.decode(bytes));
      if (isRecord(parsed) && !Array.isArray(parsed)) semantic = responseSemanticObservation(status, parsed);
    } catch {
      // HTTP status remains authoritative when a buffered body is not valid JSON.
    }
  }
  if (semantic) return { stream: false, framing_valid: true, ...semantic };
  return status >= 400
    ? {
      stream: false,
      completed: false,
      terminal_type: "http.error",
      failure_kind: null,
      framing_valid: true,
    }
    : status === 202
    ? {
      stream: false,
      completed: false,
      terminal_type: "http.accepted",
      failure_kind: null,
      framing_valid: true,
    }
    : {
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
  if (!/^(?:0|[1-9][0-9]*)$/.test(raw)) throw new Error("Response Content-Length is invalid");
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

export const inspectSentinelBufferedResponseBody = async (
  response: Response,
): Promise<SentinelClientBodyObservation | null> => {
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  try {
    bytes = await readBoundedResponseClone(response);
    return inspectSentinelBufferedResponse(
      response.status,
      response.headers.get("content-type")?.toLowerCase() ?? "",
      bytes,
    );
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

const sseEventObservation = (
  rawEvent: string,
): Omit<SentinelClientBodyObservation, "stream" | "framing_valid"> | null => {
  const data: string[] = [];
  let eventName: string | null = null;
  for (const line of rawEvent.split("\n")) {
    if (!line || line.startsWith(":")) continue;
    const separator = line.indexOf(":");
    const field = separator < 0 ? line : line.slice(0, separator);
    const value = separator < 0 ? "" : line.slice(separator + 1).replace(/^ /, "");
    if (field === "event") eventName = value;
    else if (field === "data") data.push(value);
  }
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
  if (type === "response.failed") {
    return {
      completed: false,
      terminal_type: type,
      failure_kind: errorKind(response?.error) ?? topLevelError ?? "response_failed",
    };
  }
  if (type === "response.incomplete") {
    const details = response && isRecord(response.incomplete_details) ? response.incomplete_details : null;
    return {
      completed: false,
      terminal_type: type,
      failure_kind: boundedFailureKind(details?.reason) ?? errorKind(response?.error) ?? topLevelError,
    };
  }
  if (type === "error" || isRecord(parsed.error)) {
    return { completed: false, terminal_type: "error", failure_kind: topLevelError ?? "error" };
  }
  if (type === "response.completed") {
    return { completed: true, terminal_type: type, failure_kind: null };
  }
  return null;
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

  const observeEvent = (rawEvent: string): void => {
    const data: string[] = [];
    for (const line of rawEvent.split("\n")) {
      if (!line || line.startsWith(":")) continue;
      const separator = line.indexOf(":");
      const field = separator < 0 ? line : line.slice(0, separator);
      const value = separator < 0 ? "" : line.slice(separator + 1).replace(/^ /, "");
      if (field !== "event" && field !== "data" && field !== "id" && field !== "retry") framingValid = false;
      if (field === "data") data.push(value);
    }
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
    if (terminalRank(observation.terminal_type) > terminalRank(terminal?.terminal_type ?? null)) terminal = observation;
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
        failure_kind: terminal?.failure_kind ??
          (framingValid
            ? null
            : terminalMissing
            ? termination === "read_error" ? "stream_read_error" : "missing_sse_terminal"
            : "invalid_sse_framing"),
        framing_valid: framingValid,
      };
    },
  };
};

export const inspectSentinelSse = (bytes: Uint8Array): SentinelClientBodyObservation => {
  const inspector = createSentinelSseInspector();
  inspector.push(bytes);
  return inspector.finish();
};

export const resolveSentinelClientFailureObservation = (
  internal: SentinelFailureObservation,
  body?: SentinelClientBodyObservation | null,
): SentinelClientFailureObservation => {
  const cancelled = internal.terminal_type === "cancelled";
  const syntheticTerminal = cancelled ? null : internal.synthetic_terminal_type;
  const fallbackTerminal = syntheticTerminal ??
    (internal.terminal_type === "response.completed" || internal.terminal_type === "response.failed" ||
        internal.terminal_type === "response.incomplete" || internal.terminal_type === "error" || cancelled
      ? internal.terminal_type
      : internal.status >= 400
      ? "http.error"
      : internal.completed
      ? "http.completed"
      : internal.terminal_type);
  const fallbackFailureKind = cancelled ? null : syntheticTerminal ? "server_error" : internal.failure_kind;
  return {
    status: internal.status,
    stream: body ? body.stream : internal.stream ?? false,
    completed: body ? body.completed : internal.completed,
    terminal_type: body ? body.terminal_type : fallbackTerminal,
    failure_kind: body ? body.failure_kind : fallbackFailureKind,
    framing_valid: body?.framing_valid ?? internal.stream !== true,
    provider_route: internal.provider_route,
  };
};

const isGatewayOrProviderIncompleteReason = (value: string | null): boolean =>
  value !== null &&
  /^(?:response_incomplete:)?(?:gateway|provider|upstream|server|network|timeout|deadline)[A-Za-z0-9_.:-]*$/i
    .test(value);

const isPersistableSentinelFailure = (
  observation: SentinelFailureObservation | SentinelClientFailureObservation,
): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  if ("framing_valid" in observation && observation.stream && !observation.framing_valid) return true;
  if (observation.status < 400 && observation.completed && observation.terminal_type === "response.completed") {
    return false;
  }
  if (observation.status >= 400) return true;
  if (observation.terminal_type === "response.incomplete") {
    return isGatewayOrProviderIncompleteReason(observation.failure_kind) ||
      ("synthetic_terminal_type" in observation && observation.synthetic_terminal_type !== null);
  }
  if (observation.failure_kind !== null) return true;
  if ("synthetic_terminal_type" in observation && observation.synthetic_terminal_type !== null) return true;
  if (
    observation.terminal_type === "deadline" || observation.terminal_type === "eof" ||
    observation.terminal_type === "error" || observation.terminal_type === "response.failed"
  ) return true;
  return observation.stream === true && !observation.completed && observation.terminal_type !== null &&
    observation.terminal_type !== "cancelled";
};

export const shouldPersistSentinelReplay = (
  observation: SentinelFailureObservation,
  clientObservation?: SentinelClientFailureObservation,
): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  return isPersistableSentinelFailure(observation) ||
    (clientObservation !== undefined && isPersistableSentinelFailure(clientObservation));
};

export const shouldSignalSentinelIncident = (
  observation: SentinelFailureObservation,
  clientObservation: SentinelClientFailureObservation,
): boolean => {
  if (observation.terminal_type === "cancelled") return false;
  if (clientObservation.stream && !clientObservation.framing_valid) return true;
  if (observation.status < 400 && observation.completed && observation.terminal_type === "response.completed") {
    return false;
  }
  if (observation.status >= 500) return true;
  if (isGatewayOrProviderIncompleteReason(clientObservation.failure_kind)) return true;
  if (observation.terminal_type === "response.incomplete") {
    return isGatewayOrProviderIncompleteReason(observation.failure_kind) ||
      observation.synthetic_terminal_type !== null ||
      (clientObservation.terminal_type === "response.incomplete" &&
        isGatewayOrProviderIncompleteReason(clientObservation.failure_kind));
  }
  if (observation.synthetic_terminal_type !== null || observation.failure_kind !== null) return true;
  return observation.terminal_type === "deadline" || observation.terminal_type === "eof" ||
    observation.terminal_type === "error";
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

const deriveKeyBytes = async (
  keyBytes: Uint8Array<ArrayBuffer>,
  purpose: "encryption" | "fingerprint" | "case-group",
): Promise<ArrayBuffer> => {
  const material = await crypto.subtle.importKey("raw", keyBytes, "HKDF", false, ["deriveBits"]);
  return await crypto.subtle.deriveBits(
    {
      name: "HKDF",
      hash: "SHA-256",
      salt: KEY_DERIVATION_SALT,
      info: TEXT_ENCODER.encode(purpose),
    },
    material,
    256,
  );
};

const importAesKey = async (keyBytes: Uint8Array<ArrayBuffer>): Promise<CryptoKey> =>
  await crypto.subtle.importKey(
    "raw",
    await deriveKeyBytes(keyBytes, "encryption"),
    { name: "AES-GCM" },
    false,
    ["encrypt", "decrypt"],
  );

const importHmacKey = async (
  keyBytes: Uint8Array<ArrayBuffer>,
  purpose: "fingerprint" | "case-group",
): Promise<CryptoKey> =>
  await crypto.subtle.importKey(
    "raw",
    await deriveKeyBytes(keyBytes, purpose),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );

const hmacHex = async (
  keyBytes: Uint8Array<ArrayBuffer>,
  purpose: "fingerprint" | "case-group",
  parts: readonly Uint8Array[],
): Promise<string> => {
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

const isStringRecord = (value: unknown): value is Record<string, string> =>
  isRecord(value) && Object.values(value).every((item) => typeof item === "string");

const isCompatibilityHeaders = (value: unknown): value is SentinelCompatibilityHeaders =>
  isStringRecord(value) &&
  Object.entries(value).every(([name, headerValue]) =>
    COMPATIBILITY_HEADER_NAME_SET.has(name) && headerValue.length > 0 && headerValue.trim() === headerValue
  );

const isFailureObservation = (value: unknown): value is SentinelFailureObservation =>
  isRecord(value) && typeof value.status === "number" && Number.isSafeInteger(value.status) &&
  (value.stream === null || typeof value.stream === "boolean") && typeof value.completed === "boolean" &&
  (value.terminal_type === null || typeof value.terminal_type === "string") &&
  (value.failure_kind === null || typeof value.failure_kind === "string") &&
  (value.synthetic_terminal_type === null || typeof value.synthetic_terminal_type === "string") &&
  typeof value.provider_route === "string";

const isClientFailureObservation = (value: unknown): value is SentinelClientFailureObservation =>
  isRecord(value) && typeof value.status === "number" && Number.isSafeInteger(value.status) &&
  typeof value.stream === "boolean" && typeof value.completed === "boolean" &&
  (value.terminal_type === null || typeof value.terminal_type === "string") &&
  (value.failure_kind === null || typeof value.failure_kind === "string") &&
  typeof value.framing_valid === "boolean" && typeof value.provider_route === "string";

const isReplayMetadata = (value: unknown): value is ReplayMetadata =>
  isRecord(value) && value.version === ENVELOPE_VERSION && Number.isSafeInteger(value.captured_at_ms) &&
  (value.captured_at_ms as number) >= 0 &&
  typeof value.endpoint === "string" && typeof value.method === "string" &&
  (value.content_type === null || typeof value.content_type === "string") &&
  isCompatibilityHeaders(value.compatibility_headers) && typeof value.failure_signature === "string" &&
  isFailureObservation(value.observation) && isClientFailureObservation(value.client_observation) &&
  typeof value.request_id === "string" &&
  typeof value.git_sha === "string" && typeof value.deno_revision === "string";

const randomBytes = (length: number): Uint8Array<ArrayBuffer> => crypto.getRandomValues(new Uint8Array(length));

const encryptionAdditionalData = (fingerprint: string): Uint8Array<ArrayBuffer> =>
  TEXT_ENCODER.encode(`uos-sentinel-replay-v1\0${fingerprint}`);

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
): Uint8Array<ArrayBuffer>[] => {
  const frame = (value: Uint8Array<ArrayBuffer>): Uint8Array<ArrayBuffer>[] => {
    const length = new Uint8Array(8);
    new DataView(length.buffer).setBigUint64(0, BigInt(value.byteLength), false);
    return [length, value];
  };
  const common = [
    ...frame(TEXT_ENCODER.encode(`uos-sentinel-replay-v1:${purpose}`)),
    ...frame(TEXT_ENCODER.encode(input.method)),
    ...frame(TEXT_ENCODER.encode(input.endpoint)),
    ...frame(TEXT_ENCODER.encode(stableHeaderText(input.compatibility_headers))),
  ];
  return purpose === "fingerprint"
    ? [...common, ...frame(input.body), ...frame(TEXT_ENCODER.encode(failureSignature))]
    : [...common, ...frame(input.body)];
};

const dedupeManifestKey = (value: unknown): Deno.KvKey | null => {
  if (!isRecord(value) || !Array.isArray(value.manifest_key) || value.manifest_key.length !== 7) return null;
  return value.manifest_key as Deno.KvKey;
};

const completeReplayIncidentEvent = async (
  kv: Deno.Kv,
  event: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined,
  readyAtMs: number,
  capture:
    | Readonly<{ status: "stored" | "duplicate"; fingerprint: string; manifestKey: Deno.KvKey }>
    | Readonly<{ status: "unavailable" }>,
): Promise<void> => {
  if (!event) return;
  if (!await completeSentinelIncidentFailureEvent(kv, event, readyAtMs, capture)) {
    throw new Error("Sentinel incident capture completion conflicted");
  }
};

export const persistEncryptedSentinelReplay = async (
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  dependencies: PersistDependencies,
  clientObservation: SentinelClientFailureObservation = resolveSentinelClientFailureObservation(observation),
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
  // own manifest.
  const bodySnapshot = cloneBytes(input.body);
  const snapshotInput: AcceptedSentinelReplayInput = { ...input, body: bodySnapshot };
  try {
    const now = dependencies.now?.() ?? Date.now();
    const failureSignature = sentinelFailureSignature(clientObservation);
    const fingerprint = await hmacHex(
      dependencies.keyBytes,
      "fingerprint",
      fingerprintParts(snapshotInput, failureSignature, "fingerprint"),
    );
    const caseGroupDigest = await hmacHex(
      dependencies.keyBytes,
      "case-group",
      fingerprintParts(snapshotInput, failureSignature, "case-group"),
    );
    const dedupeKey = [...SENTINEL_REPLAY_DEDUPE_PREFIX, fingerprint] as const;
    const existingDedupe = await dependencies.kv.get(dedupeKey);
    if (existingDedupe.value !== null) {
      const manifestKey = dedupeManifestKey(existingDedupe.value);
      if (!manifestKey) throw new Error("Sentinel replay dedupe record is invalid");
      await completeReplayIncidentEvent(dependencies.kv, dependencies.incidentEvent, now, {
        status: "duplicate",
        fingerprint,
        manifestKey,
      });
      return { status: "duplicate", fingerprint, manifest_key: manifestKey };
    }

    const captureId = dependencies.randomUuid?.() ?? crypto.randomUUID();
    const iv = dependencies.randomBytes?.(AES_GCM_IV_BYTES) ?? randomBytes(AES_GCM_IV_BYTES);
    if (iv.byteLength !== AES_GCM_IV_BYTES) throw new Error("Sentinel replay IV must be 12 bytes");
    const metadata: ReplayMetadata = {
      version: ENVELOPE_VERSION,
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
    };
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
    let encrypted: Uint8Array<ArrayBuffer>;
    try {
      encrypted = new Uint8Array(
        await crypto.subtle.encrypt(
          { name: "AES-GCM", iv, additionalData: encryptionAdditionalData(fingerprint) },
          await importAesKey(dependencies.keyBytes),
          compressed,
        ),
      );
    } finally {
      compressed.fill(0);
    }
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
    const cleanupChunks = async (): Promise<void> => {
      await Promise.all(
        chunks.map((_chunk, index) => dependencies.kv.delete([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index])),
      );
    };
    try {
      await Promise.all(
        chunks.map((chunk, index) =>
          dependencies.kv.set([...SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index], chunk, {
            expireIn: SENTINEL_REPLAY_TTL_MS,
          })
        ),
      );
      let operation = dependencies.kv.atomic()
        .check({ key: dedupeKey, versionstamp: null })
        .set(dedupeKey, { manifest_key: manifestKey }, { expireIn: SENTINEL_REPLAY_TTL_MS })
        .set(manifestKey, manifest, { expireIn: SENTINEL_REPLAY_TTL_MS });
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
      const committed = await operation.commit();
      if (committed.ok) return { status: "stored", manifest, manifest_key: manifestKey };
      await cleanupChunks().catch(() => {});
      const winningDedupe = await dependencies.kv.get(dedupeKey);
      const winningManifestKey = dedupeManifestKey(winningDedupe.value);
      if (!winningManifestKey) throw new Error("Sentinel replay dedupe winner is unavailable");
      await completeReplayIncidentEvent(dependencies.kv, dependencies.incidentEvent, now, {
        status: "duplicate",
        fingerprint,
        manifestKey: winningManifestKey,
      });
      return { status: "duplicate", fingerprint, manifest_key: winningManifestKey };
    } catch (error) {
      await cleanupChunks().catch(() => {});
      throw error;
    } finally {
      encrypted.fill(0);
      for (const chunk of chunks) chunk.fill(0);
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
  clientObservation?: SentinelClientFailureObservation,
): Promise<SentinelReplayPersistResult> => {
  let keyBytes: Uint8Array<ArrayBuffer> | null = null;
  let kv: Deno.Kv | null = null;
  let incidentEvent: Deno.KvEntry<SentinelIncidentFailureEvent> | undefined;
  const resolvedClientObservation = clientObservation ?? resolveSentinelClientFailureObservation(observation);
  const now = Date.now();
  try {
    if (!shouldPersistSentinelReplay(observation, resolvedClientObservation)) {
      throw new Error("A successful request cannot be persisted as a sentinel replay");
    }
    kv = await getKv();
    if (!kv) return { status: "disabled", reason: "kv_unavailable" };
    if (shouldSignalSentinelIncident(observation, resolvedClientObservation)) {
      try {
        incidentEvent = (await createSentinelIncidentFailureEventFromEnvironment(kv, now)) ?? undefined;
      } catch {
        console.warn(
          "[ai.ubq.fi] sentinel_incident",
          JSON.stringify({ status: "deferred", reason: "outbox_write_failed" }),
        );
      }
    }
    keyBytes = readReplayKeyFromEnvironment();
    if (!keyBytes) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, now, { status: "unavailable" });
      } catch {
        console.warn(
          "[ai.ubq.fi] sentinel_incident",
          JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }),
        );
      }
      return { status: "disabled", reason: "key_missing" };
    }
    try {
      return await persistEncryptedSentinelReplay(
        input,
        observation,
        { kv, keyBytes, now: () => now, incidentEvent },
        resolvedClientObservation,
      );
    } catch (error) {
      try {
        await completeReplayIncidentEvent(kv, incidentEvent, Date.now(), { status: "unavailable" });
      } catch {
        console.warn(
          "[ai.ubq.fi] sentinel_incident",
          JSON.stringify({ status: "deferred", reason: "capture_completion_failed" }),
        );
      }
      throw error;
    }
  } finally {
    keyBytes?.fill(0);
    keyBytes = null;
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
    !isRecord(value) || value.version !== ENVELOPE_VERSION || typeof value.capture_id !== "string" ||
    value.capture_id.length < 1 || value.capture_id.length > MAX_CAPTURE_ID_CHARS ||
    !CAPTURE_ID.test(value.capture_id) ||
    typeof value.fingerprint !== "string" || !HEX_DIGEST.test(value.fingerprint) ||
    typeof value.case_group_digest !== "string" || !HEX_DIGEST.test(value.case_group_digest) ||
    typeof value.captured_at_ms !== "number" || !Number.isSafeInteger(value.captured_at_ms) ||
    value.captured_at_ms < 0 || typeof value.expires_at_ms !== "number" || !Number.isSafeInteger(value.expires_at_ms) ||
    value.expires_at_ms !== value.captured_at_ms + SENTINEL_REPLAY_TTL_MS || value.algorithm !== "AES-256-GCM" ||
    value.compression !== "gzip" || typeof value.iv !== "string" || !decodedIvIsValid(value.iv) ||
    typeof value.chunk_count !== "number" || !Number.isSafeInteger(value.chunk_count) || value.chunk_count < 1 ||
    value.chunk_count > MAX_REPLAY_CHUNKS || typeof value.ciphertext_bytes !== "number" ||
    !Number.isSafeInteger(value.ciphertext_bytes) || value.ciphertext_bytes < 16 ||
    value.ciphertext_bytes > MAX_REPLAY_CIPHERTEXT_BYTES
  ) return false;
  const minimumBytes = (value.chunk_count - 1) * SENTINEL_REPLAY_CHUNK_BYTES + 1;
  return value.ciphertext_bytes >= minimumBytes &&
    value.ciphertext_bytes <= value.chunk_count * SENTINEL_REPLAY_CHUNK_BYTES;
};

const expectedChunkBytes = (manifest: SentinelReplayManifest, index: number): number =>
  index < manifest.chunk_count - 1
    ? SENTINEL_REPLAY_CHUNK_BYTES
    : manifest.ciphertext_bytes - (manifest.chunk_count - 1) * SENTINEL_REPLAY_CHUNK_BYTES;

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
        typeof encoded !== "string" || encoded.length < 1 ||
        encoded.length > Math.ceil(SENTINEL_REPLAY_CHUNK_BYTES / 3) * 4 ||
        !/^[A-Za-z0-9_-]+$/.test(encoded)
      ) return false;
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
      (_, index) => [...SENTINEL_REPLAY_CHUNK_PREFIX, manifest.capture_id, offset + index] as Deno.KvKey,
    );
    const entries = await kv.getMany<readonly Uint8Array[]>(keys);
    for (let entryIndex = 0; entryIndex < entries.length; entryIndex++) {
      const entry = entries[entryIndex]!;
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
  return suffix.length === 3 && suffix[0] === entry.value.captured_at_ms &&
    suffix[1] === entry.value.fingerprint && suffix[2] === entry.value.capture_id;
};

const manifestMatchesKey = (key: Deno.KvKey, manifest: SentinelReplayManifest): boolean => {
  const suffix = key.slice(SENTINEL_REPLAY_MANIFEST_PREFIX.length);
  return SENTINEL_REPLAY_MANIFEST_PREFIX.every((part, index) => key[index] === part) && suffix.length === 3 &&
    suffix[0] === manifest.captured_at_ms && suffix[1] === manifest.fingerprint && suffix[2] === manifest.capture_id;
};

export const listEncryptedSentinelReplays = async (
  kv: Deno.Kv,
  options: Readonly<{ afterMs: number; beforeMs: number; cursor?: string; limit?: number }>,
): Promise<Readonly<{ captures: ExportedSentinelReplayCapture[]; cursor: string }>> => {
  if (!Number.isSafeInteger(options.afterMs) || options.afterMs < 0) {
    throw new Error("Sentinel replay export start is invalid");
  }
  if (
    !Number.isSafeInteger(options.beforeMs) || options.beforeMs < options.afterMs ||
    options.beforeMs >= Number.MAX_SAFE_INTEGER
  ) {
    throw new Error("Sentinel replay export end is invalid");
  }
  if (options.limit !== undefined && options.limit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
    throw new Error("Sentinel replay export limit must be one");
  }
  if (
    options.cursor !== undefined &&
    (options.cursor.length < 1 || options.cursor.length > 2_048 || !KV_CURSOR.test(options.cursor))
  ) {
    throw new Error("Sentinel replay export cursor is invalid");
  }
  const iterator = kv.list<SentinelReplayManifest>(
    {
      prefix: SENTINEL_REPLAY_MANIFEST_PREFIX,
      start: [...SENTINEL_REPLAY_MANIFEST_PREFIX, options.afterMs],
    },
    { cursor: options.cursor, limit: SENTINEL_REPLAY_EXPORT_PAGE_LIMIT },
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
  options: Readonly<{ incidentId: string; cursor?: string; limit?: number }>,
): Promise<Readonly<{ captures: ExportedSentinelReplayCapture[]; cursor: string }>> => {
  if (!isSentinelIncidentId(options.incidentId)) throw new Error("Sentinel incident ID is invalid");
  if (options.limit !== undefined && options.limit !== SENTINEL_REPLAY_EXPORT_PAGE_LIMIT) {
    throw new Error("Sentinel replay export limit must be one");
  }
  if (
    options.cursor !== undefined &&
    (options.cursor.length < 1 || options.cursor.length > 2_048 || !KV_CURSOR.test(options.cursor))
  ) throw new Error("Sentinel replay export cursor is invalid");
  const prefix = [...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, options.incidentId] as const;
  const iterator = kv.list(
    { prefix },
    { cursor: options.cursor, limit: SENTINEL_REPLAY_EXPORT_PAGE_LIMIT },
  );
  const captures: ExportedSentinelReplayCapture[] = [];
  for await (const entry of iterator) {
    const fingerprint = entry.key.at(-1);
    if (
      entry.key.length !== prefix.length + 1 || typeof fingerprint !== "string" ||
      !/^[0-9a-f]{64}$/.test(fingerprint) || !isSentinelIncidentCaptureReference(entry.value)
    ) throw new Error("Sentinel incident replay reference is invalid");
    const manifestEntry = await kv.get<SentinelReplayManifest>(entry.value.manifest_key);
    if (
      !manifestEntry.value || !isSentinelReplayManifest(manifestEntry.value) ||
      manifestEntry.value.fingerprint !== fingerprint ||
      !manifestMatchesKey(entry.value.manifest_key, manifestEntry.value)
    ) throw new Error("Sentinel incident replay manifest is unavailable");
    const chunks = await getChunks(kv, manifestEntry.value);
    captures.push({ manifest: manifestEntry.value, chunks: chunks.map(base64UrlEncode) });
    break;
  }
  return { captures, cursor: iterator.cursor };
};

export const decryptExportedSentinelReplay = async (
  exported: ExportedSentinelReplayCapture,
  keyBytes: Uint8Array<ArrayBuffer>,
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
        ciphertext,
      ),
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
    const fingerprint = await hmacHex(
      keyBytes,
      "fingerprint",
      fingerprintParts(accepted, plaintext.failure_signature, "fingerprint"),
    );
    const caseGroupDigest = await hmacHex(
      keyBytes,
      "case-group",
      fingerprintParts(accepted, plaintext.failure_signature, "case-group"),
    );
    if (
      fingerprint !== exported.manifest.fingerprint || caseGroupDigest !== exported.manifest.case_group_digest ||
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
