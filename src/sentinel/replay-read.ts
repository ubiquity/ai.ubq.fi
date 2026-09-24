// Sentinel replay manifest reading and export, split out of src/sentinel_replay_capture.ts.

import { isSentinelIncidentCaptureReference, isSentinelIncidentId, SENTINEL_INCIDENT_CAPTURE_REF_PREFIX } from "./incident-outbox.ts";
import { base64UrlDecode, base64UrlEncode, isRecord } from "../utils.ts";
import type {
  AcceptedSentinelReplayInput,
  ExportedSentinelReplayCapture,
  SentinelReplayCaptureStatusRow,
  SentinelReplayManifest,
  SentinelReplayPlaintext,
} from "./replay-model.ts";
import {
  AES_GCM_IV_BYTES,
  CAPTURE_ID,
  ENVELOPE_VERSION,
  HEX_DIGEST,
  KV_CURSOR,
  MAX_CAPTURE_ID_CHARS,
  MAX_REPLAY_CHUNKS,
  MAX_REPLAY_CIPHERTEXT_BYTES,
  REPLAY_KEY_BYTES,
  SENTINEL_REPLAY_CHUNK_BYTES,
  SENTINEL_REPLAY_CHUNK_PREFIX,
  SENTINEL_REPLAY_EXPORT_PAGE_LIMIT,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
  SENTINEL_REPLAY_TTL_MS,
  cloneBytes,
  concatBytes,
  requestStatusKey,
} from "./replay-model.ts";
import { isSentinelReplayCaptureStatusRow, isSentinelReplayRequestId, sentinelFailureSignature } from "./replay-observation.ts";
import { decodePlaintext, encryptionAdditionalData, fingerprintParts, gunzip, hmacHex, importAesKey } from "./replay-envelope.ts";

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

export { getChunks, manifestMatchesKey };
