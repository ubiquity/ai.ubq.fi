import {
  createSentinelSseInspector,
  decodeSentinelReplayKey,
  decryptExportedSentinelReplay,
  type ExportedSentinelReplayCapture,
  inspectSentinelBufferedResponse,
  isExportedSentinelReplayCapture,
  sentinelFailureSignature,
} from "../../src/sentinel_replay_capture.ts";
import { isSentinelIncidentId } from "../../src/sentinel_incident_outbox.ts";
import type { ReplayCase, ReplayResult } from "./types.ts";

type Fetch = typeof fetch;

export const SENTINEL_MAX_ENCRYPTED_REPLAY_PAGE_BYTES = 48 * 1_024 * 1_024;
export const SENTINEL_MAX_ENCRYPTED_REPLAY_TOTAL_BYTES = 128 * 1_024 * 1_024;
export const SENTINEL_MAX_REPLAY_EXPORT_PAGES = 1_024;
export const SENTINEL_REPLAY_EXPORT_DEADLINE_MS = 10 * 60 * 1_000;
export const SENTINEL_REPLAY_EXPORT_MAX_RETRIES = 3;
export const SENTINEL_MAX_REPLAY_RESPONSE_BYTES = 16 * 1_024 * 1_024;

const SENTINEL_REPLAY_EXPORT_REQUEST_TIMEOUT_MS = 20_000;
const SENTINEL_REPLAY_EXPORT_RETRY_DELAY_MS = 250;

const defaultSleep = (milliseconds: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

class BoundedResponseError extends Error {
  constructor(readonly reason: "invalid_content_length" | "response_too_large") {
    super(reason === "response_too_large" ? "Response exceeds its size limit" : "Response Content-Length is invalid");
    this.name = "BoundedResponseError";
  }
}

const declaredContentLength = (response: Response): number | null => {
  const raw = response.headers.get("content-length");
  if (raw === null) return null;
  if (!/^(?:0|[1-9][0-9]*)$/.test(raw)) throw new BoundedResponseError("invalid_content_length");
  const parsed = Number(raw);
  if (!Number.isSafeInteger(parsed)) throw new BoundedResponseError("invalid_content_length");
  return parsed;
};

const concatBytes = (parts: readonly Uint8Array[], total: number): Uint8Array<ArrayBuffer> => {
  const output = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
};

const readBoundedResponse = async (
  response: Response,
  maxBytes: number,
  onChunk?: (chunk: Uint8Array) => void,
): Promise<Uint8Array<ArrayBuffer>> => {
  let declared: number | null;
  try {
    declared = declaredContentLength(response);
  } catch (error) {
    await response.body?.cancel().catch(() => {});
    throw error;
  }
  if (declared !== null && declared > maxBytes) {
    await response.body?.cancel().catch(() => {});
    throw new BoundedResponseError("response_too_large");
  }
  if (!response.body) return new Uint8Array();
  const reader = response.body.getReader();
  const parts: Uint8Array<ArrayBuffer>[] = [];
  let total = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value.byteLength > maxBytes - total) {
        await reader.cancel().catch(() => {});
        throw new BoundedResponseError("response_too_large");
      }
      total += value.byteLength;
      onChunk?.(value);
      if (!onChunk) parts.push(new Uint8Array(value));
    }
  } catch (error) {
    await reader.cancel(error).catch(() => {});
    throw error;
  } finally {
    reader.releaseLock();
  }
  return onChunk ? new Uint8Array() : concatBytes(parts, total);
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value);

const replayExportDeadlineError = (): Error => new Error("Sentinel replay export exceeded the overall deadline");

const isReplayExportTimeout = (error: unknown, signal: AbortSignal): boolean =>
  signal.aborted ||
  (error instanceof DOMException && (error.name === "AbortError" || error.name === "TimeoutError")) ||
  (error instanceof Error && (error.name === "AbortError" || error.name === "TimeoutError"));

const isReplayExportNetworkError = (error: unknown): boolean =>
  error instanceof TypeError || (error instanceof DOMException && error.name === "NetworkError");

const INFERENCE_ONLY_REPLAY_PATHS = new Set([
  "/v1/chat/completions",
  "/v1/responses",
  "/uos/embeddings",
]);

export const isInferenceOnlyReplayEndpoint = (endpoint: string): boolean => {
  if (!endpoint.startsWith("/")) return false;
  try {
    const base = new URL("https://sentinel-replay.invalid");
    const target = new URL(endpoint, base);
    return target.origin === base.origin && INFERENCE_ONLY_REPLAY_PATHS.has(target.pathname);
  } catch {
    return false;
  }
};

export const fetchEncryptedReplayCaptures = async (
  input: Readonly<{
    baseUrl: string;
    adminToken: string;
    afterMs: number;
    beforeMs: number;
    incidentId?: string;
    fetchImpl?: Fetch;
    now?: () => number;
    sleep?: (milliseconds: number) => Promise<void>;
    createTimeoutSignal?: (milliseconds: number) => AbortSignal;
  }>,
): Promise<ExportedSentinelReplayCapture[]> => {
  if (!input.adminToken) throw new Error("Sentinel replay export credential is missing");
  if (!Number.isSafeInteger(input.afterMs) || input.afterMs < 0) throw new Error("Replay export start is invalid");
  if (!Number.isSafeInteger(input.beforeMs) || input.beforeMs < input.afterMs) {
    throw new Error("Replay export end is invalid");
  }
  if (input.incidentId !== undefined && !isSentinelIncidentId(input.incidentId)) {
    throw new Error("Replay export incident ID is invalid");
  }
  const fetchImpl = input.fetchImpl ?? fetch;
  const now = input.now ?? Date.now;
  const sleep = input.sleep ?? defaultSleep;
  const createTimeoutSignal = input.createTimeoutSignal ?? AbortSignal.timeout;
  const base = new URL(input.baseUrl);
  const deadlineAt = now() + SENTINEL_REPLAY_EXPORT_DEADLINE_MS;
  const remainingDeadlineMs = (): number => {
    const remaining = deadlineAt - now();
    if (remaining <= 0) throw replayExportDeadlineError();
    return Math.ceil(remaining);
  };
  const retry = async (attempt: number, error: Error): Promise<void> => {
    const remaining = remainingDeadlineMs();
    if (attempt >= SENTINEL_REPLAY_EXPORT_MAX_RETRIES) throw error;
    const delay = Math.min(SENTINEL_REPLAY_EXPORT_RETRY_DELAY_MS * (2 ** attempt), remaining);
    await sleep(delay);
    remainingDeadlineMs();
  };
  const fetchPage = async (url: URL): Promise<Uint8Array<ArrayBuffer>> => {
    for (let attempt = 0; attempt <= SENTINEL_REPLAY_EXPORT_MAX_RETRIES; attempt++) {
      const signal = createTimeoutSignal(
        Math.min(SENTINEL_REPLAY_EXPORT_REQUEST_TIMEOUT_MS, remainingDeadlineMs()),
      );
      let response: Response;
      try {
        response = await fetchImpl(url, {
          method: "GET",
          redirect: "manual",
          headers: { Authorization: `Bearer ${input.adminToken}`, Accept: "application/json" },
          signal,
        });
      } catch (error) {
        if (!isReplayExportTimeout(error, signal) && !isReplayExportNetworkError(error)) throw error;
        await retry(attempt, new Error("Sentinel replay export request failed", { cause: error }));
        continue;
      }
      try {
        remainingDeadlineMs();
      } catch (error) {
        await response.body?.cancel().catch(() => {});
        throw error;
      }
      if (response.redirected || (response.status >= 300 && response.status < 400)) {
        await response.body?.cancel().catch(() => {});
        throw new Error(`Sentinel replay export rejected an HTTP redirect (${response.status})`);
      }
      if (!response.ok) {
        await response.body?.cancel().catch(() => {});
        const failure = new Error(`Sentinel replay export failed with HTTP ${response.status}`);
        if (response.status !== 429 && (response.status < 500 || response.status > 599)) throw failure;
        await retry(attempt, failure);
        continue;
      }
      let pageBytes: Uint8Array<ArrayBuffer>;
      try {
        pageBytes = await readBoundedResponse(response, SENTINEL_MAX_ENCRYPTED_REPLAY_PAGE_BYTES);
      } catch (error) {
        if (error instanceof BoundedResponseError) throw error;
        await retry(attempt, new Error("Sentinel replay export response read failed", { cause: error }));
        continue;
      }
      remainingDeadlineMs();
      return pageBytes;
    }
    throw new Error("Sentinel replay export retry bound is invalid");
  };
  const captures: ExportedSentinelReplayCapture[] = [];
  let cursor: string | null = null;
  let totalBytes = 0;
  let previousManifestKey: readonly [number, string, string] | null = null;
  const observedFingerprints = new Set<string>();
  const observedCursors = new Set<string>();
  for (let pageNumber = 0; pageNumber < SENTINEL_MAX_REPLAY_EXPORT_PAGES; pageNumber++) {
    const url = new URL("/admin/sentinel/replay-captures", base);
    url.searchParams.set("after_ms", String(input.afterMs));
    url.searchParams.set("before_ms", String(input.beforeMs));
    url.searchParams.set("limit", "1");
    if (input.incidentId) url.searchParams.set("incident_id", input.incidentId);
    if (cursor) url.searchParams.set("cursor", cursor);
    const pageBytes = await fetchPage(url);
    let parsed: unknown;
    try {
      parsed = JSON.parse(new TextDecoder().decode(pageBytes));
    } catch {
      throw new Error("Sentinel replay export returned invalid JSON");
    }
    if (
      !isRecord(parsed) || !Array.isArray(parsed.data) ||
      parsed.data.length > 1 ||
      !(parsed.cursor === null ||
        (typeof parsed.cursor === "string" && parsed.cursor.length <= 2_048 &&
          /^[A-Za-z0-9_-]+={0,2}$/.test(parsed.cursor))) ||
      !parsed.data.every(isExportedSentinelReplayCapture)
    ) {
      throw new Error("Sentinel replay export returned an invalid encrypted page");
    }
    if (parsed.cursor !== null && parsed.data.length !== 1) {
      throw new Error("Sentinel replay export returned an empty continuation page");
    }
    let acceptedManifestKey: readonly [number, string, string] | null = null;
    for (const capture of parsed.data) {
      if (
        !input.incidentId &&
        (capture.manifest.captured_at_ms < input.afterMs || capture.manifest.captured_at_ms > input.beforeMs)
      ) {
        throw new Error("Sentinel replay export returned an out-of-range manifest");
      }
      const currentKey = [
        capture.manifest.captured_at_ms,
        capture.manifest.fingerprint,
        capture.manifest.capture_id,
      ] as const;
      if (
        !input.incidentId &&
        previousManifestKey &&
        (currentKey[0] < previousManifestKey[0] ||
          (currentKey[0] === previousManifestKey[0] && currentKey[1] < previousManifestKey[1]) ||
          (currentKey[0] === previousManifestKey[0] && currentKey[1] === previousManifestKey[1] &&
            currentKey[2] <= previousManifestKey[2]))
      ) {
        throw new Error("Sentinel replay export manifests are not strictly ordered");
      }
      if (observedFingerprints.has(capture.manifest.fingerprint)) {
        throw new Error("Sentinel replay export repeated a capture fingerprint");
      }
      acceptedManifestKey = currentKey;
    }
    if (parsed.cursor !== null && observedCursors.has(parsed.cursor)) {
      throw new Error("Sentinel replay export cursor repeated");
    }
    remainingDeadlineMs();
    if (pageBytes.byteLength > SENTINEL_MAX_ENCRYPTED_REPLAY_TOTAL_BYTES - totalBytes) {
      throw new Error("Sentinel replay export exceeded the aggregate byte limit");
    }
    totalBytes += pageBytes.byteLength;
    for (const capture of parsed.data) observedFingerprints.add(capture.manifest.fingerprint);
    previousManifestKey = acceptedManifestKey ?? previousManifestKey;
    captures.push(...parsed.data);
    cursor = parsed.cursor;
    if (!cursor) return captures;
    observedCursors.add(cursor);
  }
  throw new Error("Sentinel replay export exceeded the page limit");
};

export const decryptReplayCaptures = async (
  captures: readonly ExportedSentinelReplayCapture[],
  encodedKey: string,
): Promise<ReplayCase[]> => {
  const keyBytes = decodeSentinelReplayKey(encodedKey.trim());
  if (!keyBytes) throw new Error("SENTINEL_REPLAY_KEY must encode exactly 32 bytes");
  const deduplicated = new Map<string, ReplayCase>();
  try {
    for (const capture of captures) {
      const plaintext = await decryptExportedSentinelReplay(capture, keyBytes);
      if (plaintext.method !== "POST" || !isInferenceOnlyReplayEndpoint(plaintext.endpoint)) {
        plaintext.body.fill(0);
        continue;
      }
      const prior = deduplicated.get(capture.manifest.fingerprint);
      prior?.body.fill(0);
      deduplicated.set(capture.manifest.fingerprint, {
        fingerprint: capture.manifest.fingerprint,
        case_group_digest: capture.manifest.case_group_digest,
        captured_at_ms: capture.manifest.captured_at_ms,
        endpoint: plaintext.endpoint,
        method: plaintext.method,
        content_type: plaintext.content_type,
        compatibility_headers: plaintext.compatibility_headers,
        body: plaintext.body,
        original: {
          status: plaintext.client_observation.status,
          stream: plaintext.client_observation.stream,
          framing_valid: plaintext.client_observation.framing_valid,
          completed: plaintext.client_observation.completed,
          terminal_type: plaintext.client_observation.terminal_type,
          failure_kind: plaintext.client_observation.failure_kind,
          provider_route: plaintext.client_observation.provider_route,
          failure_signature: plaintext.failure_signature,
          internal_terminal_type: plaintext.observation.terminal_type,
          internal_failure_kind: plaintext.observation.failure_kind,
          synthetic_terminal_type: plaintext.observation.synthetic_terminal_type,
        },
      });
    }
    return [...deduplicated.values()].sort((left, right) => left.captured_at_ms - right.captured_at_ms);
  } catch (error) {
    for (const replayCase of deduplicated.values()) replayCase.body.fill(0);
    throw error;
  } finally {
    keyBytes.fill(0);
  }
};

export const selectCurrentAndMatchingRegressionCases = (
  current: readonly ReplayCase[],
  retained: readonly ReplayCase[],
): ReplayCase[] => {
  const groups = new Set(current.map((item) => item.case_group_digest));
  const selected = new Map(current.map((item) => [item.fingerprint, item]));
  for (const item of retained) {
    if (groups.has(item.case_group_digest) && !selected.has(item.fingerprint)) selected.set(item.fingerprint, item);
  }
  return [...selected.values()].sort((left, right) => left.captured_at_ms - right.captured_at_ms);
};

type SseObservation = Readonly<{
  framingValid: boolean;
  terminalEvent: string | null;
  failureKind: string | null;
  completed: boolean;
}>;

export const inspectSse = (text: string): SseObservation => {
  const inspector = createSentinelSseInspector();
  inspector.push(new TextEncoder().encode(text));
  const inspected = inspector.finish();
  return {
    framingValid: inspected.framing_valid,
    terminalEvent: inspected.terminal_type,
    failureKind: inspected.failure_kind,
    completed: inspected.completed,
  };
};

const inspectSseResponse = async (response: Response): Promise<SseObservation> => {
  const inspector = createSentinelSseInspector();
  let readFailed = false;
  try {
    await readBoundedResponse(response, SENTINEL_MAX_REPLAY_RESPONSE_BYTES, (chunk) => inspector.push(chunk));
  } catch (error) {
    if (error instanceof BoundedResponseError || (error instanceof DOMException && error.name === "TimeoutError")) {
      throw error;
    }
    readFailed = true;
  }
  const inspected = inspector.finish(readFailed ? "read_error" : "eof");
  return {
    framingValid: inspected.framing_valid,
    terminalEvent: inspected.terminal_type,
    failureKind: inspected.failure_kind,
    completed: inspected.completed,
  };
};

const inspectBuffered = (status: number, contentType: string, bytes: Uint8Array): SseObservation => {
  const inspected = inspectSentinelBufferedResponse(status, contentType, bytes);
  return {
    framingValid: inspected.framing_valid,
    terminalEvent: inspected.terminal_type,
    failureKind: inspected.failure_kind,
    completed: inspected.completed,
  };
};

export const replayOneCase = async (
  input: Readonly<{
    replayCase: ReplayCase;
    previewBaseUrl: string;
    previewCredential: string;
    fetchImpl?: Fetch;
    timeoutMs?: number;
  }>,
): Promise<ReplayResult> => {
  const unavailable = (reason: string): ReplayResult => ({
    capture_fingerprint: input.replayCase.fingerprint,
    attempted: false,
    unavailable_reason: reason,
    http_status: null,
    sse_framing_valid: null,
    terminal_event: null,
    provider_route: null,
    observed_failure_signature: null,
    outcome: "unavailable",
    comparison: {
      status_matches_original: null,
      terminal_matches_original: null,
      provider_matches_original: null,
      failure_signature_matches_original: null,
      framing_matches_original: null,
    },
  });
  if (!input.previewCredential) return unavailable("preview_credential_missing");
  let target: URL;
  try {
    const base = new URL(input.previewBaseUrl);
    target = new URL(input.replayCase.endpoint, base);
    if (
      target.origin !== base.origin || input.replayCase.method !== "POST" ||
      !isInferenceOnlyReplayEndpoint(input.replayCase.endpoint)
    ) {
      return unavailable("case_target_not_inference_only");
    }
  } catch {
    return unavailable("case_target_invalid");
  }
  const headers = new Headers(input.replayCase.compatibility_headers);
  if (input.replayCase.content_type) headers.set("content-type", input.replayCase.content_type);
  headers.set("authorization", `Bearer ${input.previewCredential}`);
  try {
    const response = await (input.fetchImpl ?? fetch)(target, {
      method: "POST",
      headers,
      body: input.replayCase.body,
      redirect: "error",
      signal: AbortSignal.timeout(input.timeoutMs ?? 120_000),
    });
    const contentType = response.headers.get("content-type")?.toLowerCase() ?? "";
    const stream = contentType.includes("text/event-stream");
    const observation = stream ? await inspectSseResponse(response) : inspectBuffered(
      response.status,
      contentType,
      await readBoundedResponse(response, SENTINEL_MAX_REPLAY_RESPONSE_BYTES),
    );
    const provider = response.headers.get("x-uos-upstream")?.trim() || "gateway";
    const signature = sentinelFailureSignature({
      status: response.status,
      stream,
      completed: observation.completed,
      terminal_type: observation.terminalEvent,
      failure_kind: observation.failureKind,
      framing_valid: observation.framingValid,
      provider_route: provider,
    });
    const statusMatches = response.status === input.replayCase.original.status;
    const terminalMatches = observation.terminalEvent === input.replayCase.original.terminal_type;
    const providerMatches = provider === input.replayCase.original.provider_route;
    const signatureMatches = signature === input.replayCase.original.failure_signature;
    const framingMatches = observation.framingValid === input.replayCase.original.framing_valid;
    const improved = response.status < 400 && observation.completed && observation.framingValid;
    return {
      capture_fingerprint: input.replayCase.fingerprint,
      attempted: true,
      unavailable_reason: null,
      http_status: response.status,
      sse_framing_valid: stream ? observation.framingValid : null,
      terminal_event: observation.terminalEvent,
      provider_route: provider,
      observed_failure_signature: signature,
      outcome: improved
        ? "improved"
        : signatureMatches || (statusMatches && terminalMatches && framingMatches)
        ? "same_failure"
        : "regressed",
      comparison: {
        status_matches_original: statusMatches,
        terminal_matches_original: terminalMatches,
        provider_matches_original: providerMatches,
        failure_signature_matches_original: signatureMatches,
        framing_matches_original: framingMatches,
      },
    };
  } catch (error) {
    return unavailable(
      error instanceof BoundedResponseError
        ? error.reason
        : error instanceof DOMException && error.name === "TimeoutError"
        ? "deadline"
        : "transport_unavailable",
    );
  }
};

export const replayCases = async (
  input: Readonly<{
    cases: readonly ReplayCase[];
    previewBaseUrl: string;
    previewCredential: string;
    fetchImpl?: Fetch;
  }>,
): Promise<ReplayResult[]> => {
  const results: ReplayResult[] = [];
  for (const replayCase of input.cases) {
    results.push(await replayOneCase({ ...input, replayCase }));
  }
  return results;
};
