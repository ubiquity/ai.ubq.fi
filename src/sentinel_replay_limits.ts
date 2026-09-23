/**
 * Single source of truth for capture-owned size limits and the per-host
 * retention budget. Serialization, encryption/compression bounds, export/decode
 * and the offline reader all import these constants, so one allowed capture
 * cannot be accepted by one layer and rejected by another.
 *
 * Scope of the budget: the encoded KV payload this feature owns (base64-expanded
 * ciphertext chunks plus metadata, status, dedupe and index row overhead) and
 * its durable accounting rows. It is NOT a bound on the shared SQLite database,
 * its WAL, reusable allocated pages, host text logs, the separate incident index
 * namespace, or any other namespace's data.
 *
 * SENTINEL_REPLAY_METADATA_RESERVE_BYTES is carved out of the budget for
 * capture-owned status and eviction/expiry tombstone rows, so payload admissions
 * may use at most `budget - reserve`; see `sentinelReplayMetadataReserve`.
 */
import { MAX_ACCEPTED_JSON_BODY_BYTES } from "./request.ts";
import { SENTINEL_UPSTREAM_MAX_ATTEMPTS, SENTINEL_UPSTREAM_MAX_BYTES, SENTINEL_UPSTREAM_MAX_CHUNKS } from "./sentinel_upstream_capture.ts";

export const SENTINEL_REPLAY_MAX_REQUEST_BYTES = MAX_ACCEPTED_JSON_BODY_BYTES;
export const SENTINEL_REPLAY_MAX_UPSTREAM_BYTES = SENTINEL_UPSTREAM_MAX_BYTES;
export const SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS = SENTINEL_UPSTREAM_MAX_CHUNKS;
export const SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS = SENTINEL_UPSTREAM_MAX_ATTEMPTS;
export const SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES = 64 * 1_024;
export const SENTINEL_REPLAY_STORAGE_CHUNK_BYTES = 48 * 1_024;

/** JSON-string encoding worst case: one input byte becomes a six-character `\uXXXX` escape. */
export const SENTINEL_REPLAY_JSON_ESCAPE_FACTOR = 6;
const base64Chars = (bytes: number): number => Math.ceil(bytes / 3) * 4;

/**
 * Derived upper bound for the metadata JSON that carries the whole upstream
 * trace plus per-chunk timing, safe headers and the bounded downstream body.
 * base64(4 MiB) = 5,592,408; timing 4,096 x 16 = 65,536; per-attempt fields
 * 8 x 8 KiB = 65,536; base64(64 KiB) = 87,384; 64 KiB framing slack
 * => about 5.6 MiB, so the published 8 MiB bound is demonstrably sufficient.
 */
export const SENTINEL_REPLAY_METADATA_DERIVED_BYTES =
  base64Chars(SENTINEL_REPLAY_MAX_UPSTREAM_BYTES) +
  SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS * 16 +
  SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS * 8 * 1_024 +
  base64Chars(SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES) +
  64 * 1_024;
export const SENTINEL_REPLAY_MAX_METADATA_BYTES = 8 * 1_024 * 1_024;
export const SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES = SENTINEL_REPLAY_MAX_REQUEST_BYTES + SENTINEL_REPLAY_MAX_METADATA_BYTES + 4;
export const SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES = SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES + 1_024 * 1_024 + 16;
export const SENTINEL_REPLAY_MAX_STORED_CHUNKS = Math.ceil(SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES / SENTINEL_REPLAY_STORAGE_CHUNK_BYTES);

/**
 * Offline reader file bounds. The decoded request body is still bounded by
 * SENTINEL_REPLAY_MAX_REQUEST_BYTES; the envelope allows JSON escaping only.
 */
export const SENTINEL_REPLAY_REQUEST_FILE_MAX_BYTES = SENTINEL_REPLAY_JSON_ESCAPE_FACTOR * SENTINEL_REPLAY_MAX_REQUEST_BYTES + 1_024 * 1_024;
export const SENTINEL_REPLAY_UPSTREAM_FILE_MAX_BYTES = SENTINEL_REPLAY_MAX_METADATA_BYTES + 64 * 1_024;

/** Fixed per-host budget for capture-owned retained data, and a hard record-count bound. */
export const SENTINEL_REPLAY_BUDGET_BYTES = 1_024 * 1_024 * 1_024;
export const SENTINEL_REPLAY_MAX_RECORDS = 50_000;
/**
 * Fixed metadata reserve inside the 1 GiB budget: capture-owned status and
 * eviction-tombstone rows may never consume more than this, so payload
 * admissions always have `budget - reserve` available. See
 * `sentinelReplayMetadataReserve` for how a smaller injectable test budget
 * scales it while the production budget keeps exactly this value.
 */
export const SENTINEL_REPLAY_METADATA_RESERVE_BYTES = 64 * 1_024 * 1_024;
/** Hard bound on capture-owned status + tombstone rows, pruned oldest-first. */
export const SENTINEL_REPLAY_MAX_STATUS_RECORDS = 50_000;

/** Conservative fixed overhead per retained capture (manifest, status, dedupe and index rows). */
export const SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES = 8 * 1_024;
/** Legacy captures predate stored_bytes; their metadata was bounded by the old 256 KiB cap. */
export const SENTINEL_REPLAY_LEGACY_METADATA_BYTES = 256 * 1_024;

/** Charge the encoded KV payload: base64-expanded ciphertext plus metadata and row overhead. */
export const sentinelReplayStoredCharge = (ciphertextBytes: number, metadataBytes: number): number =>
  base64Chars(Math.max(0, ciphertextBytes)) + Math.max(0, metadataBytes) + SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;

/** Conservative pre-encryption reservation for a known plaintext size. */
export const sentinelReplayReservationBytes = (plaintextBytes: number, metadataBytes: number): number =>
  sentinelReplayStoredCharge(Math.min(SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES, Math.max(0, plaintextBytes) + 64 * 1_024 + 16), metadataBytes);
