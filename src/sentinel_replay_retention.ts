/**
 * Durable, concurrency-safe retention accounting for capture-owned data.
 *
 * One fixed per-host budget covers the encoded KV payload this feature retains
 * (base64-expanded ciphertext chunks, metadata, status/dedupe/index row
 * overhead) plus in-progress reservations. It is not a bound on the shared
 * SQLite database, WAL or unrelated namespaces.
 *
 * Source of truth: one durable accounting row per capture under
 * ["uos_ai","sentinel_replay","v1","accounting", captured_at_ms, capture_id].
 * The timestamp-first key order IS the oldest-first index; there is no second
 * index namespace. Accounting rows carry NO `expireIn`: they are the durable
 * record of retained bytes and a row stops existing only through one atomic
 * commit that deletes it and decrements the ledger in the same operation.
 *
 * The ledger under SENTINEL_REPLAY_BUDGET_LEDGER_KEY is derived cached state and
 * is never the source of truth. Every accounting mutation (admit, publish,
 * revoke, release, evict, expire, status admit/prune) is ONE kv.atomic() commit
 * that checks the exact versionstamp of BOTH the accounting/status row it
 * mutates AND the ledger, and writes both. A ledger delta is never applied in a
 * commit separate from the row change it describes.
 *
 * State machine:
 *   reserved --publish--> stored --evict--> (row deleted, ledger stored_bytes-=,
 *                                            records-=, evicted_*+=)
 *   reserved --revoke--> revoked --release--> (row deleted, reserved_bytes-=)
 *   stored --expire--> (row deleted, records-=, NO evicted_* increment)
 *
 * Fencing: a writer advances `stage` through a CAS that requires
 * state==="reserved", its own fence and an unexpired row before each bounded
 * chunk batch; a reaper revoking the same row increments the fence, so a paused
 * writer can never advance again and can never publish.
 */
import {
  isSentinelReplayCaptureStatusRow,
  isSentinelReplayManifest,
  SENTINEL_REPLAY_CHUNK_PREFIX,
  SENTINEL_REPLAY_DEDUPE_PREFIX,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
  SENTINEL_REPLAY_REQUEST_PREFIX,
  SENTINEL_REPLAY_STATUS_TTL_MS,
  SENTINEL_REPLAY_TTL_MS,
  type SentinelReplayCaptureStatusRow,
  type SentinelReplayManifest,
} from "./sentinel_replay_capture.ts";
import {
  SENTINEL_REPLAY_BUDGET_BYTES,
  SENTINEL_REPLAY_LEGACY_METADATA_BYTES,
  SENTINEL_REPLAY_MAX_RECORDS,
  SENTINEL_REPLAY_MAX_STATUS_RECORDS,
  SENTINEL_REPLAY_METADATA_RESERVE_BYTES,
  SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES,
  sentinelReplayReservationBytes,
  sentinelReplayStoredCharge,
} from "./sentinel_replay_limits.ts";

export const SENTINEL_REPLAY_BUDGET_LEDGER_KEY = ["uos_ai", "sentinel_replay", "v1", "budget"] as const;
/** Durable per-capture accounting rows; the 5th component is the captured-at millisecond. */
export const SENTINEL_REPLAY_ACCOUNTING_PREFIX = ["uos_ai", "sentinel_replay", "v1", "accounting"] as const;
/** Legacy pre-accounting-row reservations: read only to release their charge once, then deleted. */
export const SENTINEL_REPLAY_RESERVATION_PREFIX = ["uos_ai", "sentinel_replay", "v1", "reservation"] as const;
/** Bounded eviction/expiry tombstones consulted when a manifest is gone. */
export const SENTINEL_REPLAY_EVICTION_PREFIX = ["uos_ai", "sentinel_replay", "v1", "evicted"] as const;
export const SENTINEL_REPLAY_RESERVATION_TTL_MS = 120_000;
export const SENTINEL_REPLAY_EVICTION_REASON = "storage_budget";
export const SENTINEL_REPLAY_EXPIRED_REASON = "payload_expired";
export const SENTINEL_REPLAY_STORAGE_FULL_REASON = "storage_full";
export const SENTINEL_REPLAY_ACCOUNTING_REASON = "storage_accounting_in_progress";
export const SENTINEL_REPLAY_STATUS_NOT_RETAINED = "status_not_retained";
export const SENTINEL_REPLAY_CLEAN_TARGET_RATIO = 0.9;
/** Chunks written per bounded, fence-guarded staging batch. */
export const SENTINEL_REPLAY_STAGING_BATCH_CHUNKS = 32;
const EVICTION_BATCH_RECORDS = 16;
const EVICTION_MAX_CHUNK_DELETES = 512;
const BOOTSTRAP_BATCH_ENTRIES = 32;
const RESERVATION_REAP_LIMIT = 8;
const STATUS_PRUNE_BATCH = 16;
const STATUS_PRUNE_SCAN = 128;
const CAS_ATTEMPTS = 5;
const TEXT_ENCODER = new TextEncoder();
const HEX_DIGEST = /^[0-9a-f]{64}$/;

export type SentinelReplayAccountingState = "reserved" | "stored" | "evicting" | "revoked";

/** The durable per-capture accounting row. Never written with `expireIn`. */
export type SentinelReplayAccountingRow = Readonly<{
  version: 1;
  capture_id: string;
  request_id: string;
  fingerprint: string;
  bytes: number;
  state: SentinelReplayAccountingState;
  fence: number;
  created_at_ms: number;
  /** Payload TTL: after this instant the reaper reclaims the charge as `expired`. */
  expires_at_ms: number;
  /** Bounded status/tombstone TTL, independent of the payload TTL. */
  status_expires_at_ms: number;
  /** Count of completed fence-guarded staging batches. */
  stage: number;
}>;

/** A pre-accounting-row reservation; only ever read to release its charge once. */
export type SentinelReplayLegacyReservation = Readonly<{
  version: 1;
  capture_id: string;
  request_id: string;
  bytes: number;
  fence: number;
  state: "reserved" | "revoked" | "published";
  created_at_ms: number;
  expires_at_ms: number;
}>;

export type SentinelReplayBudgetLedger = Readonly<{
  version: 1;
  budget_bytes: number;
  stored_bytes: number;
  reserved_bytes: number;
  records: number;
  /** Capture-owned status + eviction-tombstone rows, bounded by the metadata reserve. */
  status_records: number;
  /** Approximate encoded bytes of those status/tombstone rows. */
  metadata_bytes: number;
  evicted_bytes: number;
  evicted_records: number;
  expired_bytes: number;
  expired_records: number;
  status_pruned_records: number;
  last_eviction_at_ms: number | null;
  last_warning_at_ms: number | null;
  last_full_at_ms: number | null;
  last_skip_reason: string | null;
  bootstrap_complete: boolean;
  /** "<prefix index>|<kv cursor>" while a resumable bootstrap sweep is in flight. */
  bootstrap_cursor: string | null;
  /** Sticky fail-closed marker set when an in-scope row could not be accounted. */
  accounting_error: string | null;
  over_budget: boolean;
}>;

export type SentinelReplayPublication = Readonly<{
  ledger_key: Deno.KvKey;
  ledger_versionstamp: string | null;
  ledger: SentinelReplayBudgetLedger;
  accounting_key: Deno.KvKey;
  accounting_versionstamp: string | null;
  accounting: SentinelReplayAccountingRow;
  /**
   * The exact request-status row the publication commit replaces. Its versionstamp
   * is checked with the ledger so the accounted metadata delta always describes
   * the row that was really replaced, never a row written by someone else.
   */
  status_key: Deno.KvKey;
  status_versionstamp: string | null;
}>;

export type SentinelReplayAdmission =
  | Readonly<{ ok: true; accounting_key: Deno.KvKey; accounting: SentinelReplayAccountingRow; charge: number }>
  | Readonly<{ ok: false; reason: typeof SENTINEL_REPLAY_STORAGE_FULL_REASON | typeof SENTINEL_REPLAY_ACCOUNTING_REASON }>;

export type SentinelReplayRetentionStatus = Readonly<{
  state: "ok" | "unavailable";
  scope: "capture_owned_kv_payload";
  budget_bytes: number | null;
  stored_bytes: number | null;
  reserved_bytes: number | null;
  records: number | null;
  metadata_bytes: number | null;
  status_records: number | null;
  evicted_records: number | null;
  evicted_bytes: number | null;
  expired_records: number | null;
  last_eviction_at_ms: number | null;
  last_warning_at_ms: number | null;
  over_budget: boolean;
  near_capacity: boolean;
  accounting_complete: boolean;
  accounting_error: string | null;
  skipped_reason: string | null;
}>;

let budgetOverrideForTest: number | null = null;
let faultInjectorForTest: ((kind: "delete" | "commit") => boolean) | null = null;

/** Test seam: an injectable small budget; production always uses the fixed default. */
export const setSentinelReplayBudgetForTest = (bytes: number | null): void => {
  budgetOverrideForTest = bytes === null ? null : Math.max(64 * 1_024, Math.trunc(bytes));
};

/** Test seam: an injected payload-delete or final-commit failure. One-shot by construction. */
export const setSentinelReplayRetentionFaultsForTest = (injector: ((kind: "delete" | "commit") => boolean) | null): void => {
  faultInjectorForTest = injector;
};

const fault = (kind: "delete" | "commit"): void => {
  if (faultInjectorForTest?.(kind)) throw new Error(`injected_${kind}_failure`);
};

export const sentinelReplayBudgetBytes = (): number => budgetOverrideForTest ?? SENTINEL_REPLAY_BUDGET_BYTES;

/**
 * Fixed 64 MiB metadata reserve inside the production 1 GiB budget, so payload
 * admissions may use at most `budget_bytes - reserve`. Budgets below 1 GiB are
 * only reachable through the injectable test seam, where the reserve scales as
 * budget/16 so both bounds stay exercisable; at exactly 1 GiB the reserve is
 * exactly SENTINEL_REPLAY_METADATA_RESERVE_BYTES.
 */
export const sentinelReplayMetadataReserve = (budgetBytes: number): number =>
  Math.max(0, Math.min(SENTINEL_REPLAY_METADATA_RESERVE_BYTES, Math.floor(Math.max(0, budgetBytes) / 16)));

/** Payload admissions may use at most this much of the budget. */
export const sentinelReplayPayloadBudgetBytes = (budgetBytes: number): number => Math.max(0, budgetBytes - sentinelReplayMetadataReserve(budgetBytes));

export const sentinelReplayAccountingKey = (capturedAtMs: number, captureId: string): Deno.KvKey => [
  ...SENTINEL_REPLAY_ACCOUNTING_PREFIX,
  capturedAtMs,
  captureId,
];

export const sentinelReplayManifestKey = (manifest: SentinelReplayManifest): Deno.KvKey => [
  ...SENTINEL_REPLAY_MANIFEST_PREFIX,
  manifest.captured_at_ms,
  manifest.fingerprint,
  manifest.capture_id,
];

/** The bounded eviction tombstone an owner lookup consults when a manifest is gone. */
export const sentinelReplayEvictionKey = (fingerprint: string): Deno.KvKey => [...SENTINEL_REPLAY_EVICTION_PREFIX, fingerprint];

export const sentinelReplayRequestStatusKey = (requestId: string): Deno.KvKey => [...SENTINEL_REPLAY_REQUEST_PREFIX, requestId];

/** Conservative charge for a retained manifest; legacy rows predate stored_bytes. */
export const sentinelReplayManifestCharge = (manifest: SentinelReplayManifest): number => {
  if (typeof manifest.stored_bytes === "number" && Number.isSafeInteger(manifest.stored_bytes) && manifest.stored_bytes > 0) return manifest.stored_bytes;
  return sentinelReplayStoredCharge(manifest.ciphertext_bytes, SENTINEL_REPLAY_LEGACY_METADATA_BYTES) + SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
};

const emptyLedger = (budgetBytes: number): SentinelReplayBudgetLedger => ({
  version: 1,
  budget_bytes: budgetBytes,
  stored_bytes: 0,
  reserved_bytes: 0,
  records: 0,
  status_records: 0,
  metadata_bytes: 0,
  evicted_bytes: 0,
  evicted_records: 0,
  expired_bytes: 0,
  expired_records: 0,
  status_pruned_records: 0,
  last_eviction_at_ms: null,
  last_warning_at_ms: null,
  last_full_at_ms: null,
  last_skip_reason: null,
  bootstrap_complete: false,
  bootstrap_cursor: null,
  accounting_error: null,
  over_budget: false,
});

const counter = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

/**
 * A present ledger is only accepted when every counter is a non-negative safe
 * integer and every marker has its declared shape. A malformed ledger is
 * reported as corrupt, never silently replaced with an empty ledger.
 */
const isLedger = (value: unknown): value is SentinelReplayBudgetLedger => {
  if (typeof value !== "object" || value === null) return false;
  const row = value as Record<string, unknown>;
  if (row.version !== 1) return false;
  for (const field of [
    "budget_bytes",
    "stored_bytes",
    "reserved_bytes",
    "records",
    "status_records",
    "metadata_bytes",
    "evicted_bytes",
    "evicted_records",
    "expired_bytes",
    "expired_records",
    "status_pruned_records",
  ]) {
    if (!counter(row[field])) return false;
  }
  if (typeof row.bootstrap_complete !== "boolean" || typeof row.over_budget !== "boolean") return false;
  for (const field of ["last_eviction_at_ms", "last_warning_at_ms", "last_full_at_ms"]) {
    if (row[field] !== null && !counter(row[field])) return false;
  }
  for (const field of ["last_skip_reason", "bootstrap_cursor", "accounting_error"]) {
    if (row[field] !== null && typeof row[field] !== "string") return false;
  }
  return true;
};

const isAccountingRow = (value: unknown): value is SentinelReplayAccountingRow => {
  if (typeof value !== "object" || value === null) return false;
  const row = value as Record<string, unknown>;
  return (
    row.version === 1 &&
    typeof row.capture_id === "string" &&
    row.capture_id.length > 0 &&
    row.capture_id.length <= 128 &&
    typeof row.request_id === "string" &&
    typeof row.fingerprint === "string" &&
    HEX_DIGEST.test(row.fingerprint) &&
    counter(row.bytes) &&
    row.bytes > 0 &&
    (row.state === "reserved" || row.state === "stored" || row.state === "evicting" || row.state === "revoked") &&
    counter(row.fence) &&
    (row.fence as number) >= 1 &&
    counter(row.created_at_ms) &&
    counter(row.expires_at_ms) &&
    counter(row.status_expires_at_ms) &&
    counter(row.stage)
  );
};

/** The key shape must agree with the row it holds before anything derived is touched. */
const accountingKeyMatches = (key: Deno.KvKey, row: SentinelReplayAccountingRow): boolean =>
  key.length === SENTINEL_REPLAY_ACCOUNTING_PREFIX.length + 2 && key[3] === "accounting" && key[4] === row.created_at_ms && key[5] === row.capture_id;

const manifestKeyMatches = (key: Deno.KvKey, manifest: SentinelReplayManifest): boolean =>
  key.length === SENTINEL_REPLAY_MANIFEST_PREFIX.length + 3 &&
  key[3] === "manifest" &&
  key[4] === manifest.captured_at_ms &&
  key[5] === manifest.fingerprint &&
  key[6] === manifest.capture_id;

const isLegacyReservation = (value: unknown): value is SentinelReplayLegacyReservation => {
  if (typeof value !== "object" || value === null) return false;
  const row = value as Record<string, unknown>;
  return (
    row.version === 1 &&
    typeof row.capture_id === "string" &&
    counter(row.bytes) &&
    counter(row.fence) &&
    (row.state === "reserved" || row.state === "revoked" || row.state === "published")
  );
};

export const sentinelReplayStatusMetadataBytes = (value: unknown): number => {
  try {
    return TEXT_ENCODER.encode(JSON.stringify(value)).byteLength + SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
  } catch {
    return SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
  }
};

/**
 * Bytes an already-stored status/tombstone row contributes to the metadata bound.
 * A missing row contributes nothing, so replacing a row costs only its difference.
 */
const storedStatusMetadataBytes = (value: unknown): number => (value === null || value === undefined ? 0 : sentinelReplayStatusMetadataBytes(value));

type LedgerState =
  | Readonly<{ kind: "absent"; ledger: SentinelReplayBudgetLedger; versionstamp: null }>
  | Readonly<{ kind: "ok"; ledger: SentinelReplayBudgetLedger; versionstamp: string }>
  | Readonly<{ kind: "corrupt"; versionstamp: string | null }>;

const readLedger = async (kv: Deno.Kv, budgetBytes: number): Promise<LedgerState> => {
  const entry = await kv.get<SentinelReplayBudgetLedger>(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  if (entry.value === null) return { kind: "absent", ledger: emptyLedger(budgetBytes), versionstamp: null };
  if (!isLedger(entry.value)) return { kind: "corrupt", versionstamp: entry.versionstamp };
  return { kind: "ok", ledger: entry.value, versionstamp: entry.versionstamp };
};

const withBudget = (ledger: SentinelReplayBudgetLedger, budgetBytes: number): SentinelReplayBudgetLedger =>
  ledger.budget_bytes === budgetBytes ? ledger : { ...ledger, budget_bytes: budgetBytes };

/** Public read-only snapshot for the admin surface and tests; `null` means corrupt. */
export const readSentinelReplayLedgerSnapshot = async (
  kv: Deno.Kv,
  budgetBytes?: number
): Promise<Readonly<{ ledger: SentinelReplayBudgetLedger; versionstamp: string | null }> | null> => {
  const budget = Math.max(64 * 1_024, Math.trunc(budgetBytes ?? sentinelReplayBudgetBytes()));
  const state = await readLedger(kv, budget);
  if (state.kind === "corrupt") return null;
  return { ledger: withBudget(state.ledger, budget), versionstamp: state.versionstamp };
};

const deleteChunkBatch = async (kv: Deno.Kv, captureId: string, limit: number): Promise<Readonly<{ deleted: number; remaining: number }>> => {
  if (limit <= 0) return { deleted: 0, remaining: 1 };
  const keys: Deno.KvKey[] = [];
  for await (const entry of kv.list({ prefix: [...SENTINEL_REPLAY_CHUNK_PREFIX, captureId] }, { limit: limit + 1 })) keys.push(entry.key);
  const batch = keys.slice(0, limit);
  for (const key of batch) {
    fault("delete");
    await kv.delete(key);
  }
  return { deleted: batch.length, remaining: keys.length - batch.length };
};

const countChunks = async (kv: Deno.Kv, captureId: string): Promise<number> => {
  const prefix = [...SENTINEL_REPLAY_CHUNK_PREFIX, captureId];
  for await (const entry of kv.list({ prefix }, { limit: 1 })) {
    // Only a row that really sits under this capture's chunk prefix proves the
    // payload survives; an unrelated stray key must not hold the charge.
    if (entry.key.length === prefix.length + 1) return 1;
  }
  return 0;
};

/**
 * Ensure the resumable sweep has finished before any counter mutation. The sweep
 * counts every in-scope row itself, so while it is pending no other writer may
 * also count a row, or the same row is charged twice. Returns the fresh ledger
 * when accounting is authoritative, and null when the ledger is corrupt or the
 * sweep is still incomplete, in which case no counter may move.
 */
const bootstrapBeforeMutation = async (kv: Deno.Kv, budgetBytes: number, nowMs: number): Promise<SentinelReplayBudgetLedger | null> => {
  const state = await readLedger(kv, budgetBytes);
  if (state.kind === "corrupt") return null;
  const ledger = withBudget(state.ledger, budgetBytes);
  if (ledger.bootstrap_complete) return ledger;
  const bootstrapped = await runBootstrapBatch(kv, ledger, state.versionstamp, budgetBytes, nowMs);
  return bootstrapped.bootstrap_complete ? bootstrapped : null;
};

/**
 * Admit one bounded status/tombstone row through a CAS on the ledger's
 * status_records/metadata_bytes counters. When either bound is exceeded the
 * oldest capture-owned status/tombstone rows are pruned first.
 */
export const admitSentinelReplayStatusMetadata = async (
  kv: Deno.Kv,
  input: Readonly<{ key: Deno.KvKey; row: unknown; now_ms: number; ttl_ms: number; budget_bytes?: number }>
): Promise<boolean> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(input.budget_bytes ?? sentinelReplayBudgetBytes()));
  const reserve = sentinelReplayMetadataReserve(budgetBytes);
  const bytes = sentinelReplayStatusMetadataBytes(input.row);
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    // The sweep counts every in-scope row, so until it has finished no row may
    // also be counted by its own admission, or the same row is charged twice.
    if ((await bootstrapBeforeMutation(kv, budgetBytes, input.now_ms)) === null) return false;
    await pruneCaptureOwnedStatusMetadata(kv, input.now_ms, 1, bytes, 1, budgetBytes);
    // The bound tracks the LIVE row set, so replacing the row already stored at
    // this key adds one record and the byte difference, never a second full
    // charge. Checking the exact versionstamp of the replaced row keeps that
    // delta true while another writer touches the same key.
    const [current, existing] = await Promise.all([readLedger(kv, budgetBytes), kv.get(input.key)]);
    if (current.kind === "corrupt") return false;
    const ledger = withBudget(current.ledger, budgetBytes);
    const addedRecords = existing.value === null ? 1 : 0;
    const addedBytes = bytes - storedStatusMetadataBytes(existing.value);
    if (ledger.status_records + addedRecords > SENTINEL_REPLAY_MAX_STATUS_RECORDS || ledger.metadata_bytes + addedBytes > reserve) return false;
    const next: SentinelReplayBudgetLedger = {
      ...ledger,
      status_records: ledger.status_records + addedRecords,
      metadata_bytes: Math.max(0, ledger.metadata_bytes + addedBytes),
    };
    const committed = await kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: current.versionstamp })
      .check({ key: input.key, versionstamp: existing.versionstamp })
      .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, next)
      .set(input.key, input.row, { expireIn: input.ttl_ms })
      .commit();
    if (committed.ok) return true;
  }
  return false;
};

type StatusCandidate = Readonly<{ key: Deno.KvKey; versionstamp: string; bytes: number; captured_at_ms: number }>;

const statusCandidates = async (kv: Deno.Kv): Promise<StatusCandidate[]> => {
  const candidates: StatusCandidate[] = [];
  for await (const entry of kv.list({ prefix: SENTINEL_REPLAY_REQUEST_PREFIX }, { limit: STATUS_PRUNE_SCAN })) {
    if (!isSentinelReplayCaptureStatusRow(entry.value)) continue;
    candidates.push({
      key: entry.key,
      versionstamp: entry.versionstamp,
      bytes: sentinelReplayStatusMetadataBytes(entry.value),
      captured_at_ms: entry.value.captured_at_ms,
    });
  }
  for await (const entry of kv.list({ prefix: SENTINEL_REPLAY_EVICTION_PREFIX }, { limit: STATUS_PRUNE_SCAN })) {
    const value = entry.value as { evicted_at_ms?: unknown } | null;
    if (typeof value !== "object" || value === null) continue;
    candidates.push({
      key: entry.key,
      versionstamp: entry.versionstamp,
      bytes: sentinelReplayStatusMetadataBytes(value),
      captured_at_ms: typeof value.evicted_at_ms === "number" ? value.evicted_at_ms : 0,
    });
  }
  return candidates.sort((left, right) => left.captured_at_ms - right.captured_at_ms);
};

/** Prune the oldest capture-owned status/tombstone rows until `extra` fits the bound. */
export const pruneCaptureOwnedStatusMetadata = async (
  kv: Deno.Kv,
  nowMs: number,
  extraRecords: number,
  extraBytes: number,
  maxDeletes: number,
  budgetBytesOverride?: number
): Promise<number> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(budgetBytesOverride ?? sentinelReplayBudgetBytes()));
  const reserve = sentinelReplayMetadataReserve(budgetBytes);
  const initial = await readLedger(kv, budgetBytes);
  if (initial.kind === "corrupt") return 0;
  const ledger = withBudget(initial.ledger, budgetBytes);
  if (ledger.status_records + extraRecords <= SENTINEL_REPLAY_MAX_STATUS_RECORDS && ledger.metadata_bytes + extraBytes <= reserve) return 0;
  let deleted = 0;
  for (const candidate of await statusCandidates(kv)) {
    if (deleted >= maxDeletes) break;
    const state = await readLedger(kv, budgetBytes);
    if (state.kind === "corrupt") break;
    const current = withBudget(state.ledger, budgetBytes);
    if (current.status_records + extraRecords <= SENTINEL_REPLAY_MAX_STATUS_RECORDS && current.metadata_bytes + extraBytes <= reserve) break;
    const next: SentinelReplayBudgetLedger = {
      ...current,
      status_records: Math.max(0, current.status_records - 1),
      metadata_bytes: Math.max(0, current.metadata_bytes - candidate.bytes),
      status_pruned_records: current.status_pruned_records + 1,
      last_warning_at_ms: nowMs,
    };
    const committed = await kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: state.versionstamp })
      .check({ key: candidate.key, versionstamp: candidate.versionstamp })
      .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, next)
      .delete(candidate.key)
      .commit();
    if (committed.ok) deleted += 1;
  }
  return deleted;
};

/** One atomic commit: the row change and the ledger delta it describes. */
const commitAccountingMutation = async (
  kv: Deno.Kv,
  key: Deno.KvKey,
  rowVersionstamp: string | null,
  mutate: (ledger: SentinelReplayBudgetLedger) => SentinelReplayBudgetLedger,
  applyRow: (operation: Deno.AtomicOperation) => Deno.AtomicOperation,
  budgetBytes: number
): Promise<boolean> => {
  const state = await readLedger(kv, budgetBytes);
  if (state.kind === "corrupt") return false;
  const ledger = withBudget(state.ledger, budgetBytes);
  const next = mutate(ledger);
  fault("commit");
  const committed = await applyRow(
    kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: state.versionstamp })
      .check({ key, versionstamp: rowVersionstamp })
      .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, next)
  ).commit();
  return committed.ok;
};

/** Fenced staging: advance `stage` before a batch of chunks may be written. */
export const advanceSentinelReplayStagingFence = async (
  kv: Deno.Kv,
  input: Readonly<{ accounting_key: Deno.KvKey; fence: number; now_ms: number; budget_bytes?: number }>
): Promise<Readonly<{ ok: true; stage: number }> | Readonly<{ ok: false; reason: "revoked" | "expired" | "missing" }>> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(input.budget_bytes ?? sentinelReplayBudgetBytes()));
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<SentinelReplayAccountingRow>(input.accounting_key);
    if (!isAccountingRow(entry.value) || !accountingKeyMatches(input.accounting_key, entry.value)) {
      return { ok: false, reason: "missing" };
    }
    const row = entry.value;
    if (row.state === "revoked") return { ok: false, reason: "revoked" };
    if (row.state !== "reserved") return { ok: false, reason: "missing" };
    if (row.fence !== input.fence) return { ok: false, reason: "revoked" };
    if (row.expires_at_ms <= input.now_ms) return { ok: false, reason: "expired" };
    const state = await readLedger(kv, budgetBytes);
    if (state.kind === "corrupt") return { ok: false, reason: "missing" };
    const next: SentinelReplayAccountingRow = { ...row, stage: row.stage + 1 };
    const committed = await kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: state.versionstamp })
      .check({ key: input.accounting_key, versionstamp: entry.versionstamp })
      .set(input.accounting_key, next)
      .commit();
    if (committed.ok) return { ok: true, stage: next.stage };
  }
  return { ok: false, reason: "missing" };
};

/** Everything one publish attempt must re-verify before it may write anything. */
type PublishAttemptContext = Readonly<{
  accountingKey: Deno.KvKey;
  accounting: SentinelReplayAccountingRow;
  actualCharge: number;
  payloadBudget: number;
  reserve: number;
  /** Net record and byte movement of the status row this attempt replaces. */
  statusRecordsDelta: number;
  statusBytesDelta: number;
  nowMs: number;
}>;

/**
 * Read the metadata bounds and, when the record cap or the byte reserve would be
 * exceeded, prune the oldest capture-owned status rows first. Returns the metadata
 * reserve, or null when the ledger is corrupt and cannot be trusted.
 */
const prepareStatusMetadataRoom = async (kv: Deno.Kv, budgetBytes: number, statusBytes: number, nowMs: number): Promise<number | null> => {
  const state = await readLedger(kv, budgetBytes);
  if (state.kind === "corrupt") return null;
  const ledger = withBudget(state.ledger, budgetBytes);
  const reserve = sentinelReplayMetadataReserve(budgetBytes);
  const tooManyRecords = ledger.status_records + 1 > SENTINEL_REPLAY_MAX_STATUS_RECORDS;
  const tooManyBytes = ledger.metadata_bytes + statusBytes > reserve;
  if (tooManyRecords || tooManyBytes) await pruneCaptureOwnedStatusMetadata(kv, nowMs, 1, statusBytes, STATUS_PRUNE_BATCH, budgetBytes);
  return reserve;
};

/**
 * Re-verify the reservation against the live row and ledger for one publish
 * attempt. The row must still be this writer's unexpired `reserved` row with its
 * original identity, the actual charge must fit both the reservation that was
 * already held back and the payload budget, and the status row must fit the
 * metadata reserve. Returns null when the reservation may no longer publish.
 */
const publishAttemptRow = (
  entry: Deno.KvEntryMaybe<SentinelReplayAccountingRow>,
  ledger: SentinelReplayBudgetLedger,
  context: PublishAttemptContext
): SentinelReplayAccountingRow | null => {
  if (!isAccountingRow(entry.value) || !accountingKeyMatches(context.accountingKey, entry.value)) return null;
  const row = entry.value;
  if (row.state !== "reserved") return null;
  if (row.fence !== context.accounting.fence) return null;
  if (row.expires_at_ms <= context.nowMs) return null;
  if (row.created_at_ms !== context.accounting.created_at_ms) return null;
  if (!Number.isSafeInteger(context.actualCharge) || context.actualCharge <= 0) return null;
  if (context.actualCharge > row.bytes) return null;
  if (ledger.reserved_bytes < row.bytes) return null;
  // The published charge must fit what the reservation already held back: the
  // reserved charge is released in the same commit that stores the actual one.
  if (ledger.stored_bytes + ledger.reserved_bytes - row.bytes + context.actualCharge > context.payloadBudget) return null;
  if (ledger.status_records + context.statusRecordsDelta > SENTINEL_REPLAY_MAX_STATUS_RECORDS) return null;
  if (ledger.metadata_bytes + context.statusBytesDelta > context.reserve) return null;
  return row;
};

/**
 * Read the fresh ledger and accounting row for a publish attempt. Returns null
 * unless the row is still `reserved` at the writer's fence, unexpired, and the
 * actual charge fits both the row's own bound and the payload budget.
 */
export const prepareSentinelReplayPublication = async (
  kv: Deno.Kv,
  accounting: SentinelReplayAccountingRow,
  accountingKey: Deno.KvKey,
  actualCharge: number,
  options: Readonly<{ now_ms: number; status_key: Deno.KvKey; status_bytes?: number; budget_bytes?: number }>
): Promise<SentinelReplayPublication | null> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(options.budget_bytes ?? sentinelReplayBudgetBytes()));
  const payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
  const statusBytes = Math.max(0, Math.trunc(options.status_bytes ?? 0));
  const reserve = await prepareStatusMetadataRoom(kv, budgetBytes, statusBytes, options.now_ms);
  if (reserve === null) return null;
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    const [entry, ledgerState, statusEntry] = await Promise.all([
      kv.get<SentinelReplayAccountingRow>(accountingKey),
      readLedger(kv, budgetBytes),
      kv.get(options.status_key),
    ]);
    if (ledgerState.kind === "corrupt") return null;
    const ledger = withBudget(ledgerState.ledger, budgetBytes);
    // Publishing replaces the request own status row, so the metadata bound moves
    // by the difference instead of charging a second full row for it.
    const statusRecordsDelta = statusEntry.value === null ? 1 : 0;
    const statusBytesDelta = statusBytes - storedStatusMetadataBytes(statusEntry.value);
    const row = publishAttemptRow(entry, ledger, {
      accountingKey,
      accounting,
      actualCharge,
      payloadBudget,
      reserve,
      statusRecordsDelta,
      statusBytesDelta,
      nowMs: options.now_ms,
    });
    if (row === null) return null;
    const stored = ledger.stored_bytes + actualCharge;
    const nextLedger: SentinelReplayBudgetLedger = {
      ...ledger,
      reserved_bytes: Math.max(0, ledger.reserved_bytes - row.bytes),
      stored_bytes: stored,
      records: ledger.records + 1,
      status_records: Math.max(0, ledger.status_records + statusRecordsDelta),
      metadata_bytes: Math.max(0, ledger.metadata_bytes + statusBytesDelta),
      over_budget: stored > payloadBudget,
      last_warning_at_ms: stored > payloadBudget ? options.now_ms : ledger.last_warning_at_ms,
    };
    // The short reservation lease becomes the payload TTL only here, at publish.
    const nextRow: SentinelReplayAccountingRow = {
      ...row,
      state: "stored",
      bytes: actualCharge,
      expires_at_ms: options.now_ms + SENTINEL_REPLAY_TTL_MS,
      status_expires_at_ms: options.now_ms + SENTINEL_REPLAY_STATUS_TTL_MS,
    };
    return {
      ledger_key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY,
      ledger_versionstamp: ledgerState.versionstamp,
      ledger: nextLedger,
      accounting_key: accountingKey,
      accounting_versionstamp: entry.versionstamp,
      accounting: nextRow,
      status_key: options.status_key,
      status_versionstamp: statusEntry.versionstamp,
    };
  }
  return null;
};

/**
 * Fence a `reserved` row so its writer can never stage or publish again, and
 * report the row cleanup should act on. A row that already published (`stored`)
 * or was already claimed (`evicting`) is NOT revoked here: releasing its charge is
 * the ordinary eviction/TTL path's job, never this cleanup's.
 */
const revokeReservedAccounting = async (
  kv: Deno.Kv,
  accounting: SentinelReplayAccountingRow,
  accountingKey: Deno.KvKey
): Promise<Readonly<{ row: SentinelReplayAccountingRow; revoked: boolean }>> => {
  if (accounting.state === "revoked") return { row: accounting, revoked: true };
  if (accounting.state !== "reserved") return { row: accounting, revoked: false };
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<SentinelReplayAccountingRow>(accountingKey);
    if (!isAccountingRow(entry.value) || entry.value.fence !== accounting.fence) break;
    if (entry.value.state === "revoked") return { row: entry.value, revoked: true };
    if (entry.value.state === "stored") return { row: entry.value, revoked: false };
    if (entry.value.state !== "reserved") break;
    const next: SentinelReplayAccountingRow = { ...entry.value, state: "revoked", fence: entry.value.fence + 1 };
    const committed = await kv.atomic().check({ key: accountingKey, versionstamp: entry.versionstamp }).set(accountingKey, next).commit();
    if (committed.ok) return { row: next, revoked: true };
  }
  return { row: accounting, revoked: false };
};

/**
 * Revoke a reservation or resume an `evicting` row: fence it, delete its staged
 * chunks in bounded batches, and release the charge ONLY when a fresh list over
 * the chunk prefix returns zero entries. A partial deletion leaves the row
 * revoked/evicting with its charge held for a later pass.
 */
export const abandonSentinelReplayAccounting = async (
  kv: Deno.Kv,
  accounting: SentinelReplayAccountingRow,
  accountingKey: Deno.KvKey,
  options: Readonly<{ now_ms: number; max_chunk_deletes?: number; budget_bytes?: number }>
): Promise<Readonly<{ revoked: boolean; deleted_chunks: number; released: boolean; remaining: number }>> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(options.budget_bytes ?? sentinelReplayBudgetBytes()));
  const maxDeletes = Math.max(1, options.max_chunk_deletes ?? EVICTION_MAX_CHUNK_DELETES);
  const { row: current, revoked } = await revokeReservedAccounting(kv, accounting, accountingKey);
  if (!revoked) return { revoked: false, deleted_chunks: 0, released: false, remaining: 0 };
  const cleanup = await deleteChunkBatch(kv, current.capture_id, maxDeletes);
  const remaining = cleanup.remaining > 0 ? cleanup.remaining : await countChunks(kv, current.capture_id);
  if (remaining > 0) return { revoked: true, deleted_chunks: cleanup.deleted, released: false, remaining };
  // Release the charge only in the same commit that deletes the row, and only
  // when a fresh listing proves no chunk survives.
  let released = false;
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<SentinelReplayAccountingRow>(accountingKey);
    if (!isAccountingRow(entry.value) || entry.value.fence !== current.fence) break;
    // Bind the narrowed row: TypeScript does not carry a property narrowing into
    // the mutation closure, and no assertion should be needed for that.
    const releasedRow: SentinelReplayAccountingRow = entry.value;
    released = await commitAccountingMutation(
      kv,
      accountingKey,
      entry.versionstamp,
      (ledger) => ({ ...ledger, reserved_bytes: Math.max(0, ledger.reserved_bytes - releasedRow.bytes) }),
      (operation) => operation.delete(accountingKey),
      budgetBytes
    );
    if (released) break;
  }
  return { revoked: true, deleted_chunks: cleanup.deleted, released, remaining: 0 };
};

/** Legacy reservation release: read once, delete chunks, then delete the row. */
const releaseLegacyReservation = async (kv: Deno.Kv, key: Deno.KvKey, reservation: SentinelReplayLegacyReservation, budgetBytes: number): Promise<void> => {
  if (reservation.state === "published") {
    await kv.delete(key);
    return;
  }
  const cleanup = await deleteChunkBatch(kv, reservation.capture_id, EVICTION_MAX_CHUNK_DELETES);
  if (cleanup.remaining > 0) return;
  const entry = await kv.get<SentinelReplayLegacyReservation>(key);
  if (!isLegacyReservation(entry.value)) return;
  await commitAccountingMutation(
    kv,
    key,
    entry.versionstamp,
    (ledger) => ({ ...ledger, reserved_bytes: Math.max(0, ledger.reserved_bytes - reservation.bytes) }),
    (operation) => operation.delete(key),
    budgetBytes
  );
};

/**
 * Find fenced/abandoned rows to clean up with bounded work. A revoked row keeps
 * its original capture timestamp, so it can sit anywhere in the oldest-first
 * prefix: scanning only the oldest window would never reach a fresh revoke on a
 * busy host, and scanning only the newest would strand an old one. Both ends of
 * the prefix are swept in bounded windows, without adding a second index.
 */
const reapCandidates = async (kv: Deno.Kv, nowMs: number, limit: number): Promise<Readonly<{ key: Deno.KvKey; row: SentinelReplayAccountingRow }>[]> => {
  const found = new Map<string, Readonly<{ key: Deno.KvKey; row: SentinelReplayAccountingRow }>>();
  const isCandidate = (row: SentinelReplayAccountingRow): boolean => row.state === "revoked" || (row.state === "reserved" && row.expires_at_ms <= nowMs);
  for (const reverse of [false, true]) {
    for await (const entry of kv.list<SentinelReplayAccountingRow>({ prefix: SENTINEL_REPLAY_ACCOUNTING_PREFIX }, { limit: limit * 4, reverse })) {
      if (!isAccountingRow(entry.value) || !accountingKeyMatches(entry.key, entry.value)) continue;
      if (!isCandidate(entry.value)) continue;
      found.set(entry.key.join("\u0000"), { key: entry.key, row: entry.value });
      if (found.size >= limit) break;
    }
    if (found.size >= limit) break;
  }
  return [...found.values()].slice(0, limit);
};

const reapAccountingRows = async (kv: Deno.Kv, nowMs: number, budgetBytes: number): Promise<number> => {
  const candidates = await reapCandidates(kv, nowMs, RESERVATION_REAP_LIMIT);
  let reaped = 0;
  for (const candidate of candidates) {
    const result = await abandonSentinelReplayAccounting(kv, candidate.row, candidate.key, { now_ms: nowMs, budget_bytes: budgetBytes });
    if (result.revoked) reaped += 1;
  }
  // A victim whose final commit failed keeps its charge and is resumed here.
  reaped += await resumeEvictingRows(kv, nowMs, budgetBytes, RESERVATION_REAP_LIMIT);
  return reaped;
};

const reapLegacyReservations = async (kv: Deno.Kv, budgetBytes: number): Promise<number> => {
  let reaped = 0;
  for await (const entry of kv.list<SentinelReplayLegacyReservation>({ prefix: SENTINEL_REPLAY_RESERVATION_PREFIX }, { limit: RESERVATION_REAP_LIMIT })) {
    if (!isLegacyReservation(entry.value)) continue;
    await releaseLegacyReservation(kv, entry.key, entry.value, budgetBytes);
    reaped += 1;
  }
  return reaped;
};

/**
 * Prefixes in capture-owned key order. Built lazily: capture.ts imports this
 * module, so a top-level array here would read capture.ts bindings before that
 * module body has initialized them. The order is significant — "accounting"
 * sorts before "manifest", so the accounting sweep is always complete before a
 * manifest is checked for an existing accounting row.
 */
const bootstrapPrefixes = (): readonly Deno.KvKey[] => [
  SENTINEL_REPLAY_ACCOUNTING_PREFIX,
  SENTINEL_REPLAY_CHUNK_PREFIX,
  SENTINEL_REPLAY_DEDUPE_PREFIX,
  SENTINEL_REPLAY_EVICTION_PREFIX,
  SENTINEL_REPLAY_MANIFEST_PREFIX,
  SENTINEL_REPLAY_REQUEST_PREFIX,
  SENTINEL_REPLAY_RESERVATION_PREFIX,
];

type BootstrapDelta = Readonly<{
  stored_bytes: number;
  reserved_bytes: number;
  records: number;
  status_records: number;
  metadata_bytes: number;
  invalid: boolean;
  legacy_reaped: number;
}>;

const emptyDelta = (): BootstrapDelta => ({
  stored_bytes: 0,
  reserved_bytes: 0,
  records: 0,
  status_records: 0,
  metadata_bytes: 0,
  invalid: false,
  legacy_reaped: 0,
});

const addDelta = (delta: BootstrapDelta, patch: Partial<BootstrapDelta>): BootstrapDelta => ({
  stored_bytes: delta.stored_bytes + (patch.stored_bytes ?? 0),
  reserved_bytes: delta.reserved_bytes + (patch.reserved_bytes ?? 0),
  records: delta.records + (patch.records ?? 0),
  status_records: delta.status_records + (patch.status_records ?? 0),
  metadata_bytes: delta.metadata_bytes + (patch.metadata_bytes ?? 0),
  invalid: delta.invalid || Boolean(patch.invalid),
  legacy_reaped: delta.legacy_reaped + (patch.legacy_reaped ?? 0),
});

const parseCursor = (cursor: string | null): Readonly<{ index: number; inner: string | null }> => {
  if (cursor === null) return { index: 0, inner: null };
  const separator = cursor.indexOf("|");
  if (separator < 0) return { index: 0, inner: null };
  const index = Number.parseInt(cursor.slice(0, separator), 10);
  return { index: Number.isSafeInteger(index) && index >= 0 ? index : 0, inner: cursor.slice(separator + 1) || null };
};

/** Accounting rows own the durable charge, so the sweep counts them directly. */
const bootstrapAccountingDelta = (key: Deno.KvKey, value: unknown, delta: BootstrapDelta): BootstrapDelta => {
  if (!isAccountingRow(value) || !accountingKeyMatches(key, value)) return addDelta(delta, { invalid: true });
  if (value.state === "stored" || value.state === "evicting") return addDelta(delta, { stored_bytes: value.bytes, records: 1 });
  return addDelta(delta, { reserved_bytes: value.bytes });
};

/** A manifest without its own accounting row is a pre-accounting capture, charged conservatively. */
const bootstrapManifestDelta = async (kv: Deno.Kv, key: Deno.KvKey, value: unknown, delta: BootstrapDelta): Promise<BootstrapDelta> => {
  if (!isSentinelReplayManifest(value) || !manifestKeyMatches(key, value)) return addDelta(delta, { invalid: true });
  const accounting = await kv.get(sentinelReplayAccountingKey(value.captured_at_ms, value.capture_id));
  if (isAccountingRow(accounting.value)) return delta;
  return addDelta(delta, { stored_bytes: sentinelReplayManifestCharge(value), records: 1 });
};

/** Request status rows are capture-owned metadata and consume the metadata reserve. */
const bootstrapStatusDelta = (value: unknown, delta: BootstrapDelta): BootstrapDelta => {
  if (!isSentinelReplayCaptureStatusRow(value)) return addDelta(delta, { invalid: true });
  return addDelta(delta, { status_records: 1, metadata_bytes: sentinelReplayStatusMetadataBytes(value) });
};

/** Eviction and expiry tombstones are capture-owned metadata too. */
const bootstrapTombstoneDelta = (value: unknown, delta: BootstrapDelta): BootstrapDelta => {
  const tombstone = value as { fingerprint?: unknown; reason?: unknown } | null;
  if (typeof tombstone !== "object" || tombstone === null || typeof tombstone.fingerprint !== "string" || typeof tombstone.reason !== "string") {
    return addDelta(delta, { invalid: true });
  }
  return addDelta(delta, { status_records: 1, metadata_bytes: sentinelReplayStatusMetadataBytes(tombstone) });
};

/** Legacy reservation rows are read only to release their own charge once. */
const bootstrapLegacyDelta = async (kv: Deno.Kv, key: Deno.KvKey, value: unknown, delta: BootstrapDelta, budgetBytes: number): Promise<BootstrapDelta> => {
  if (!isLegacyReservation(value)) return addDelta(delta, { invalid: true });
  await releaseLegacyReservation(kv, key, value, budgetBytes);
  return addDelta(delta, { legacy_reaped: 1 });
};

/** Chunk rows carry no independent charge, but a malformed key must fail closed. */
const bootstrapChunkDelta = (key: Deno.KvKey, delta: BootstrapDelta): BootstrapDelta => {
  if (key.length !== SENTINEL_REPLAY_CHUNK_PREFIX.length + 2 || typeof key[4] !== "string" || !counter(key[5])) return addDelta(delta, { invalid: true });
  return delta;
};

/** A dedupe row must reference a well-formed manifest key. */
const bootstrapDedupeDelta = (value: unknown, delta: BootstrapDelta): BootstrapDelta => {
  const dedupe = value as { manifest_key?: unknown } | null;
  if (typeof dedupe !== "object" || dedupe === null || !Array.isArray(dedupe.manifest_key) || dedupe.manifest_key.length !== 7) {
    return addDelta(delta, { invalid: true });
  }
  return delta;
};

/**
 * Fold one scanned in-scope row into the running bootstrap delta. Rows that do
 * not match their own prefix's contract are counted as invalid rather than
 * trusted or deleted on a guess, which keeps discovery fail-closed.
 */
const bootstrapEntryDelta = async (
  kv: Deno.Kv,
  prefix: Deno.KvKey,
  entry: Deno.KvEntry<unknown>,
  delta: BootstrapDelta,
  budgetBytes: number
): Promise<BootstrapDelta> => {
  if (prefix === SENTINEL_REPLAY_ACCOUNTING_PREFIX) return bootstrapAccountingDelta(entry.key, entry.value, delta);
  if (prefix === SENTINEL_REPLAY_MANIFEST_PREFIX) return await bootstrapManifestDelta(kv, entry.key, entry.value, delta);
  if (prefix === SENTINEL_REPLAY_REQUEST_PREFIX) return bootstrapStatusDelta(entry.value, delta);
  if (prefix === SENTINEL_REPLAY_EVICTION_PREFIX) return bootstrapTombstoneDelta(entry.value, delta);
  if (prefix === SENTINEL_REPLAY_RESERVATION_PREFIX) return await bootstrapLegacyDelta(kv, entry.key, entry.value, delta, budgetBytes);
  if (prefix === SENTINEL_REPLAY_CHUNK_PREFIX) return bootstrapChunkDelta(entry.key, delta);
  return bootstrapDedupeDelta(entry.value, delta);
};

/** Outcome of sweeping one bootstrap prefix within the remaining entry budget. */
type BootstrapSweep = Readonly<{ delta: BootstrapDelta; scanned: number; cursor: string | null; exhausted: boolean }>;

/**
 * Sweep one prefix up to `budget` entries. `exhausted` means the sweep consumed
 * its whole budget mid-prefix, so the caller must resume from `cursor` at the
 * same prefix instead of advancing.
 */
const sweepBootstrapPrefix = async (
  kv: Deno.Kv,
  prefix: Deno.KvKey,
  cursor: string | null,
  budget: number,
  delta: BootstrapDelta,
  budgetBytes: number
): Promise<BootstrapSweep> => {
  const iterator = kv.list({ prefix }, { cursor: cursor ?? undefined, limit: budget });
  let scanned = 0;
  for await (const entry of iterator) {
    scanned += 1;
    delta = await bootstrapEntryDelta(kv, prefix, entry, delta, budgetBytes);
    if (scanned >= budget) return { delta, scanned, cursor: iterator.cursor, exhausted: true };
  }
  return { delta, scanned, cursor: null, exhausted: false };
};

/**
 * One bounded, resumable bootstrap sweep across EVERY capture-owned prefix that
 * holds retained bytes or metadata. It counts scanned entries, not just valid
 * records: an invalid or unreadable in-scope row marks accounting incomplete so
 * new admissions stay refused instead of trusting a zero.
 */
const runBootstrapBatch = async (
  kv: Deno.Kv,
  ledger: SentinelReplayBudgetLedger,
  versionstamp: string | null,
  budgetBytes: number,
  nowMs: number
): Promise<SentinelReplayBudgetLedger> => {
  let { index, inner } = parseCursor(ledger.bootstrap_cursor);
  const prefixes = bootstrapPrefixes();
  // A fresh sweep clears the sticky error so a removed corrupt row can resolve it.
  let error = ledger.bootstrap_cursor === null ? null : ledger.accounting_error;
  let delta = emptyDelta();
  let scanned = 0;
  while (index < prefixes.length && scanned < BOOTSTRAP_BATCH_ENTRIES) {
    const sweep = await sweepBootstrapPrefix(kv, prefixes[index], inner, BOOTSTRAP_BATCH_ENTRIES - scanned, delta, budgetBytes);
    delta = sweep.delta;
    scanned += sweep.scanned;
    if (sweep.exhausted) {
      // Keep this prefix and its cursor: the next pass resumes exactly here.
      inner = sweep.cursor;
      break;
    }
    index += 1;
    inner = null;
  }
  const complete = index >= prefixes.length;
  if (delta.invalid) error = "invalid_in_scope_row";
  const next: SentinelReplayBudgetLedger = {
    ...ledger,
    budget_bytes: budgetBytes,
    stored_bytes: ledger.stored_bytes + delta.stored_bytes,
    reserved_bytes: ledger.reserved_bytes + delta.reserved_bytes,
    records: ledger.records + delta.records,
    status_records: ledger.status_records + delta.status_records,
    metadata_bytes: ledger.metadata_bytes + delta.metadata_bytes,
    bootstrap_complete: complete && error === null,
    bootstrap_cursor: complete ? null : `${index}|${inner ?? ""}`,
    accounting_error: error,
    over_budget: ledger.stored_bytes + delta.stored_bytes > sentinelReplayPayloadBudgetBytes(budgetBytes),
    last_warning_at_ms: ledger.stored_bytes + delta.stored_bytes > sentinelReplayPayloadBudgetBytes(budgetBytes) ? nowMs : ledger.last_warning_at_ms,
  };
  fault("commit");
  const committed = await kv.atomic().check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp }).set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, next).commit();
  if (committed.ok) return next;
  const reread = await readLedger(kv, budgetBytes);
  if (reread.kind === "corrupt") return ledger;
  return withBudget(reread.ledger, budgetBytes);
};

const recordSkipReason = async (kv: Deno.Kv, reason: string | null, nowMs: number, budgetBytes: number): Promise<void> => {
  for (let attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
    const state = await readLedger(kv, budgetBytes);
    if (state.kind === "corrupt") return;
    const ledger = withBudget(state.ledger, budgetBytes);
    const next: SentinelReplayBudgetLedger = {
      ...ledger,
      last_skip_reason: reason,
      last_full_at_ms: reason === SENTINEL_REPLAY_STORAGE_FULL_REASON ? nowMs : ledger.last_full_at_ms,
      last_warning_at_ms: reason === null ? ledger.last_warning_at_ms : nowMs,
    };
    const committed = await kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: state.versionstamp })
      .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, next)
      .commit();
    if (committed.ok) return;
  }
};

/**
 * Delete a claimed victim's manifest and, only when the dedupe row still
 * references that exact manifest key, its dedupe row. The CAS is what stops an
 * old capture's eviction from deleting a newer capture that shares the
 * fingerprint, and a manifest that no longer matches the claim is left alone.
 */
const deleteClaimedVictimPayload = async (kv: Deno.Kv, row: SentinelReplayAccountingRow, manifestKey: Deno.KvKey): Promise<void> => {
  const manifestEntry = await kv.get<SentinelReplayManifest>(manifestKey);
  if (!isSentinelReplayManifest(manifestEntry.value)) return;
  if (!manifestKeyMatches(manifestKey, manifestEntry.value) || manifestEntry.value.fingerprint !== row.fingerprint) return;
  fault("delete");
  await kv.delete(manifestKey);
  const dedupeKey = [...SENTINEL_REPLAY_DEDUPE_PREFIX, row.fingerprint];
  const dedupeEntry = await kv.get<{ manifest_key?: unknown }>(dedupeKey);
  if (!Array.isArray(dedupeEntry.value?.manifest_key)) return;
  if (dedupeEntry.value.manifest_key.length !== 7) return;
  if (dedupeEntry.value.manifest_key.join("\u0000") !== manifestKey.join("\u0000")) return;
  await kv.atomic().check({ key: dedupeKey, versionstamp: dedupeEntry.versionstamp }).delete(dedupeKey).commit();
};

/**
 * The ledger transition for one finalized victim: release exactly its charge and
 * count it as either evicted or expired. Status/tombstone counters move only when
 * `statusFits`, because the metadata reserve is a hard bound even during cleanup.
 */
const finalizedLedger = (
  ledger: SentinelReplayBudgetLedger,
  row: SentinelReplayAccountingRow,
  kind: "evicted" | "expired",
  nowMs: number,
  budgetBytes: number,
  statusRecordsDelta: number,
  metadataBytesDelta: number
): SentinelReplayBudgetLedger => {
  const stored = Math.max(0, ledger.stored_bytes - row.bytes);
  return {
    ...ledger,
    stored_bytes: stored,
    records: Math.max(0, ledger.records - 1),
    status_records: Math.max(0, ledger.status_records + statusRecordsDelta),
    metadata_bytes: Math.max(0, ledger.metadata_bytes + metadataBytesDelta),
    evicted_bytes: kind === "evicted" ? ledger.evicted_bytes + row.bytes : ledger.evicted_bytes,
    evicted_records: kind === "evicted" ? ledger.evicted_records + 1 : ledger.evicted_records,
    expired_bytes: kind === "expired" ? ledger.expired_bytes + row.bytes : ledger.expired_bytes,
    expired_records: kind === "expired" ? ledger.expired_records + 1 : ledger.expired_records,
    last_eviction_at_ms: kind === "evicted" ? nowMs : ledger.last_eviction_at_ms,
    over_budget: stored > sentinelReplayPayloadBudgetBytes(budgetBytes),
  };
};

/**
 * Delete one already-claimed victim's payload and release its charge in a single
 * atomic commit. The charge is released only after a fresh listing proves the
 * chunk prefix is empty, so a partial deletion never frees capacity.
 */
const finalizeClaimedVictim = async (
  kv: Deno.Kv,
  key: Deno.KvKey,
  row: SentinelReplayAccountingRow,
  kind: "evicted" | "expired",
  nowMs: number,
  budgetBytes: number
): Promise<Readonly<{ finalized: boolean; chunks: number }>> => {
  const cleanup = await deleteChunkBatch(kv, row.capture_id, EVICTION_MAX_CHUNK_DELETES);
  const remaining = cleanup.remaining > 0 ? cleanup.remaining : await countChunks(kv, row.capture_id);
  if (remaining > 0) return { finalized: false, chunks: cleanup.deleted };
  const manifestKey = sentinelReplayManifestKey({
    version: 1,
    capture_id: row.capture_id,
    fingerprint: row.fingerprint,
    case_group_digest: "",
    captured_at_ms: row.created_at_ms,
    expires_at_ms: row.expires_at_ms,
    algorithm: "AES-256-GCM",
    compression: "gzip",
    iv: "",
    chunk_count: 0,
    ciphertext_bytes: 0,
  });
  await deleteClaimedVictimPayload(kv, row, manifestKey);
  const statusRow: SentinelReplayCaptureStatusRow = {
    version: 1,
    request_id: row.request_id,
    status: kind === "evicted" ? "evicted" : "expired",
    reason: kind === "evicted" ? SENTINEL_REPLAY_EVICTION_REASON : SENTINEL_REPLAY_EXPIRED_REASON,
    captured_at_ms: row.created_at_ms,
    manifest_key: null,
    fingerprint: null,
    expires_at_ms: null,
  };
  const tombstone = {
    version: 1,
    fingerprint: row.fingerprint,
    request_id: row.request_id,
    evicted_at_ms: nowMs,
    reason: kind === "evicted" ? SENTINEL_REPLAY_EVICTION_REASON : SENTINEL_REPLAY_EXPIRED_REASON,
  };
  const statusKey = sentinelReplayRequestStatusKey(row.request_id);
  const tombstoneKey = sentinelReplayEvictionKey(row.fingerprint);
  const statusBytes = sentinelReplayStatusMetadataBytes(statusRow) + sentinelReplayStatusMetadataBytes(tombstone);
  await pruneCaptureOwnedStatusMetadata(kv, nowMs, 2, statusBytes, STATUS_PRUNE_BATCH, budgetBytes);
  // Read the rows this commit really replaces or deletes. The bound tracks the
  // live row set, so re-creating the victim status row and adding its tombstone
  // may move it only by the difference; the versionstamps checked below keep that
  // delta true while another writer touches those keys.
  const [ledgerState, existingStatus, existingTombstone] = await Promise.all([readLedger(kv, budgetBytes), kv.get(statusKey), kv.get(tombstoneKey)]);
  if (ledgerState.kind === "corrupt") return { finalized: false, chunks: cleanup.deleted };
  const ledger = withBudget(ledgerState.ledger, budgetBytes);
  const existingStatusBytes = storedStatusMetadataBytes(existingStatus.value);
  const existingTombstoneBytes = storedStatusMetadataBytes(existingTombstone.value);
  const statusPresent = existingStatus.value === null ? 0 : 1;
  const tombstonePresent = existingTombstone.value === null ? 0 : 1;
  // The metadata bound is hard even during cleanup: when the reserve cannot take
  // the tombstone, the victim's bytes are still released below, but the bounded
  // status/tombstone rows are dropped instead of exceeding the bound.
  const metadataReserve = sentinelReplayMetadataReserve(budgetBytes);
  // Room for both rows means both are written; otherwise the status row is
  // deleted and any existing tombstone survives exactly as it was.
  const statusFits =
    ledger.status_records + 2 - statusPresent - tombstonePresent <= SENTINEL_REPLAY_MAX_STATUS_RECORDS &&
    ledger.metadata_bytes + statusBytes - existingStatusBytes - existingTombstoneBytes <= metadataReserve;
  const afterRecords = statusFits ? 2 : tombstonePresent;
  const afterBytes = statusFits ? statusBytes : existingTombstoneBytes;
  const statusRecordsDelta = afterRecords - statusPresent - tombstonePresent;
  const metadataBytesDelta = afterBytes - existingStatusBytes - existingTombstoneBytes;
  const nextLedger = finalizedLedger(ledger, row, kind, nowMs, budgetBytes, statusRecordsDelta, metadataBytesDelta);
  const entry = await kv.get<SentinelReplayAccountingRow>(key);
  if (!isAccountingRow(entry.value) || entry.value.state !== "evicting") return { finalized: false, chunks: cleanup.deleted };
  fault("commit");
  let operation = kv
    .atomic()
    .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: ledgerState.versionstamp })
    .check({ key, versionstamp: entry.versionstamp })
    .check({ key: statusKey, versionstamp: existingStatus.versionstamp })
    .check({ key: tombstoneKey, versionstamp: existingTombstone.versionstamp })
    .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, nextLedger)
    .delete(key)
    .delete(statusKey);
  if (statusFits) {
    operation = operation
      .set(statusKey, statusRow, { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS })
      .set(tombstoneKey, tombstone, { expireIn: SENTINEL_REPLAY_STATUS_TTL_MS });
  }
  const committed = await operation.commit();
  // A commit failure leaves the victim in `evicting` with its charge held for a
  // later pass; its bytes were fully removed, so nothing is double-charged.
  return { finalized: committed.ok, chunks: cleanup.deleted };
};

/**
 * Claim one stored victim for exactly-once deletion, then finalize it. The claim
 * commit is what makes eviction exactly-once: exactly one caller can move
 * stored -> evicting at this row versionstamp, and a loser must move on without
 * touching the victim.
 */
const claimVictim = async (kv: Deno.Kv, key: Deno.KvKey, row: SentinelReplayAccountingRow, budgetBytes: number): Promise<boolean> => {
  // Re-read before claiming: a stale victim whose row was already deleted or
  // already claimed must never be resurrected by a `versionstamp: null` check.
  const current = await kv.get<SentinelReplayAccountingRow>(key);
  if (
    !isAccountingRow(current.value) ||
    !accountingKeyMatches(key, current.value) ||
    current.value.state !== "stored" ||
    current.value.fence !== row.fence ||
    current.value.bytes !== row.bytes
  ) {
    return false;
  }
  // Bind the narrowed row: TypeScript does not carry a property narrowing into
  // the mutation closure, and no assertion should be needed for that.
  const claimed: SentinelReplayAccountingRow = current.value;
  return await commitAccountingMutation(
    kv,
    key,
    current.versionstamp,
    (ledger) => ledger,
    (operation) => operation.set(key, { ...claimed, state: "evicting", fence: row.fence + 1 }),
    budgetBytes
  );
};

/**
 * Resume an `evicting` victim whose final commit failed after its bytes were
 * removed. The claim kind is unambiguous because eviction only ever claims
 * unexpired rows: an evicting row past its payload TTL was claimed by the TTL
 * reclamation pass and stays `expired`, never `evicted`.
 */
const resumeEvictingRows = async (kv: Deno.Kv, nowMs: number, budgetBytes: number, limit: number): Promise<number> => {
  const victims = await selectVictims(kv, limit, (row) => row.state === "evicting", nowMs);
  let resumed = 0;
  for (const victim of victims) {
    const kind = victim.row.expires_at_ms <= nowMs ? "expired" : "evicted";
    const result = await finalizeClaimedVictim(kv, victim.key, victim.row, kind, nowMs, budgetBytes);
    if (result.finalized) resumed += 1;
  }
  return resumed;
};

const selectVictims = async (
  kv: Deno.Kv,
  limit: number,
  predicate: (row: SentinelReplayAccountingRow, nowMs: number) => boolean,
  nowMs: number
): Promise<Readonly<{ key: Deno.KvKey; row: SentinelReplayAccountingRow }>[]> => {
  const victims: Readonly<{ key: Deno.KvKey; row: SentinelReplayAccountingRow }>[] = [];
  // Accounting keys are timestamp-first, so listing this prefix IS the
  // oldest-first index; no second index namespace is needed.
  for await (const entry of kv.list<SentinelReplayAccountingRow>({ prefix: SENTINEL_REPLAY_ACCOUNTING_PREFIX }, { limit: limit * 8 })) {
    if (!isAccountingRow(entry.value) || !accountingKeyMatches(entry.key, entry.value)) continue;
    if (!predicate(entry.value, nowMs)) continue;
    victims.push({ key: entry.key, row: entry.value });
    if (victims.length >= limit) break;
  }
  return victims;
};

/**
 * Evict oldest stored captures until the ledger reaches the target. Each victim
 * is claimed by exactly one caller before anything is deleted, and its charge is
 * released only when its chunk prefix is proven empty.
 */
export const evictSentinelReplays = async (
  kv: Deno.Kv,
  options: Readonly<{ target_bytes: number; max_records?: number; max_chunk_deletes?: number; now_ms: number; budget_bytes?: number }>
): Promise<Readonly<{ records: number; bytes: number; chunks: number }>> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(options.budget_bytes ?? sentinelReplayBudgetBytes()));
  const maxRecords = Math.max(1, Math.min(EVICTION_BATCH_RECORDS, options.max_records ?? EVICTION_BATCH_RECORDS));
  // Expired payloads belong to the TTL reclamation pass, which reports them as
  // `expired`; eviction only claims rows whose payload TTL has not passed, so a
  // resumed `evicting` row's claim kind is unambiguous.
  const victims = await selectVictims(kv, maxRecords, (row, nowMs) => row.state === "stored" && row.expires_at_ms > nowMs, options.now_ms);
  let records = 0;
  let bytes = 0;
  let chunks = 0;
  for (const victim of victims) {
    const state = await readLedger(kv, budgetBytes);
    if (state.kind === "corrupt") break;
    const ledger = withBudget(state.ledger, budgetBytes);
    if (ledger.stored_bytes <= options.target_bytes) break;
    const remainingDeletes = Math.max(1, (options.max_chunk_deletes ?? EVICTION_MAX_CHUNK_DELETES) - chunks);
    if (!(await claimVictim(kv, victim.key, victim.row, budgetBytes))) continue;
    const result = await finalizeClaimedVictim(kv, victim.key, victim.row, "evicted", options.now_ms, budgetBytes);
    chunks += result.chunks;
    if (!result.finalized) {
      if (remainingDeletes <= 0) break;
      continue;
    }
    records += 1;
    bytes += victim.row.bytes;
  }
  return { records, bytes, chunks };
};

/**
 * TTL reclamation: accounting rows outlive their payload TTL, so a maintenance
 * pass claims rows whose payload already expired and releases the charge with
 * records-1 and NO evicted_* increment, reporting `expired`/`payload_expired`.
 */
export const reclaimExpiredSentinelReplays = async (
  kv: Deno.Kv,
  options: Readonly<{ now_ms: number; max_records?: number; budget_bytes?: number }>
): Promise<Readonly<{ records: number; bytes: number }>> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(options.budget_bytes ?? sentinelReplayBudgetBytes()));
  const maxRecords = Math.max(1, Math.min(EVICTION_BATCH_RECORDS, options.max_records ?? EVICTION_BATCH_RECORDS));
  const victims = await selectVictims(kv, maxRecords, (row, nowMs) => row.state === "stored" && row.expires_at_ms <= nowMs, options.now_ms);
  let records = 0;
  let bytes = 0;
  for (const victim of victims) {
    if (!(await claimVictim(kv, victim.key, victim.row, budgetBytes))) continue;
    const result = await finalizeClaimedVictim(kv, victim.key, victim.row, "expired", options.now_ms, budgetBytes);
    if (!result.finalized) continue;
    records += 1;
    bytes += victim.row.bytes;
  }
  return { records, bytes };
};

/**
 * Bounded cleanup for an over-budget admission: reclaim payload-TTL expiry first,
 * then evict oldest-first toward the clean target. Returns whether any capacity
 * was actually reclaimed, so a refusal is only reported when nothing moved.
 */
const reclaimAdmissionCapacity = async (kv: Deno.Kv, budgetBytes: number, payloadBudget: number, charge: number, nowMs: number): Promise<boolean> => {
  const target = Math.floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO) - charge;
  const expired = await reclaimExpiredSentinelReplays(kv, { now_ms: nowMs, budget_bytes: budgetBytes });
  const evicted = await evictSentinelReplays(kv, { target_bytes: Math.max(0, target), now_ms: nowMs, budget_bytes: budgetBytes });
  return evicted.records > 0 || expired.records > 0;
};

/**
 * Reserve capacity for one capture before any chunk is written: the accounting
 * row (state=reserved) and the ledger's reserved_bytes increment commit
 * together. Refuses with storage_full when bounded cleanup cannot admit it.
 */
export const reserveSentinelReplayCapacity = async (
  kv: Deno.Kv,
  input: Readonly<{
    capture_id: string;
    request_id: string;
    plaintext_bytes: number;
    metadata_bytes: number;
    fingerprint: string;
    now_ms: number;
    budget_bytes?: number;
  }>
): Promise<SentinelReplayAdmission> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(input.budget_bytes ?? sentinelReplayBudgetBytes()));
  const payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
  const charge = sentinelReplayReservationBytes(input.plaintext_bytes, input.metadata_bytes);
  for (let round = 0; round < 4; round += 1) {
    await reapAccountingRows(kv, input.now_ms, budgetBytes);
    const state = await readLedger(kv, budgetBytes);
    if (state.kind === "corrupt") {
      await recordSkipReason(kv, SENTINEL_REPLAY_ACCOUNTING_REASON, input.now_ms, budgetBytes);
      return { ok: false, reason: SENTINEL_REPLAY_ACCOUNTING_REASON };
    }
    const ledger = withBudget(state.ledger, budgetBytes);
    if (!ledger.bootstrap_complete) {
      const bootstrapped = await runBootstrapBatch(kv, ledger, state.versionstamp, budgetBytes, input.now_ms);
      if (!bootstrapped.bootstrap_complete) {
        await recordSkipReason(kv, SENTINEL_REPLAY_ACCOUNTING_REASON, input.now_ms, budgetBytes);
        return { ok: false, reason: SENTINEL_REPLAY_ACCOUNTING_REASON };
      }
      continue;
    }
    const overBudget = ledger.stored_bytes + ledger.reserved_bytes + charge > payloadBudget || ledger.records + 1 > SENTINEL_REPLAY_MAX_RECORDS;
    if (overBudget) {
      if (!(await reclaimAdmissionCapacity(kv, budgetBytes, payloadBudget, charge, input.now_ms))) {
        await recordSkipReason(kv, SENTINEL_REPLAY_STORAGE_FULL_REASON, input.now_ms, budgetBytes);
        return { ok: false, reason: SENTINEL_REPLAY_STORAGE_FULL_REASON };
      }
      continue;
    }
    const row: SentinelReplayAccountingRow = {
      version: 1,
      capture_id: input.capture_id,
      request_id: input.request_id,
      fingerprint: input.fingerprint,
      bytes: charge,
      state: "reserved",
      fence: 1,
      created_at_ms: input.now_ms,
      // A short staging lease: a crashed writer's charge is reaped promptly. The
      // payload TTL is written only when publish turns this row into `stored`.
      expires_at_ms: input.now_ms + SENTINEL_REPLAY_RESERVATION_TTL_MS,
      status_expires_at_ms: input.now_ms + SENTINEL_REPLAY_STATUS_TTL_MS,
      stage: 0,
    };
    const key = sentinelReplayAccountingKey(row.created_at_ms, row.capture_id);
    const nextLedger: SentinelReplayBudgetLedger = {
      ...ledger,
      reserved_bytes: ledger.reserved_bytes + charge,
      last_skip_reason: null,
    };
    fault("commit");
    const committed = await kv
      .atomic()
      .check({ key: SENTINEL_REPLAY_BUDGET_LEDGER_KEY, versionstamp: state.versionstamp })
      .check({ key, versionstamp: null })
      .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, nextLedger)
      .set(key, row)
      .commit();
    if (committed.ok) return { ok: true, accounting_key: key, accounting: row, charge };
  }
  await recordSkipReason(kv, SENTINEL_REPLAY_STORAGE_FULL_REASON, input.now_ms, budgetBytes);
  return { ok: false, reason: SENTINEL_REPLAY_STORAGE_FULL_REASON };
};

/** Bounded maintenance: reap, resume bootstrap, reclaim expired payloads, evict if needed. */
export const runSentinelReplayRetentionMaintenance = async (
  kv: Deno.Kv,
  options: Readonly<{ now_ms: number; budget_bytes?: number }>
): Promise<SentinelReplayRetentionStatus> => {
  const budgetBytes = Math.max(64 * 1_024, Math.trunc(options.budget_bytes ?? sentinelReplayBudgetBytes()));
  const payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
  // The resumable sweep runs FIRST and gates every counter mutation below. It
  // counts each in-scope row itself, so a release or reclamation that moved a
  // counter while the sweep was still pending would let that row be counted a
  // second time once the sweep reached it.
  if ((await bootstrapBeforeMutation(kv, budgetBytes, options.now_ms)) === null) return await readSentinelReplayRetentionStatus(kv, budgetBytes);
  await reapAccountingRows(kv, options.now_ms, budgetBytes);
  await reapLegacyReservations(kv, budgetBytes);
  await reclaimExpiredSentinelReplays(kv, { now_ms: options.now_ms, budget_bytes: budgetBytes });
  const current = await readLedger(kv, budgetBytes);
  if (current.kind === "ok" && current.ledger.stored_bytes + current.ledger.reserved_bytes > payloadBudget) {
    await evictSentinelReplays(kv, {
      target_bytes: Math.floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO),
      now_ms: options.now_ms,
      budget_bytes: budgetBytes,
    });
  }
  return await readSentinelReplayRetentionStatus(kv, budgetBytes);
};

/** Current retention status for the admin surface; `unavailable` is never reported as zero. */
export const readSentinelReplayRetentionStatus = async (kv: Deno.Kv, budgetBytes?: number): Promise<SentinelReplayRetentionStatus> => {
  const budget = Math.max(64 * 1_024, Math.trunc(budgetBytes ?? sentinelReplayBudgetBytes()));
  try {
    const state = await readLedger(kv, budget);
    if (state.kind === "corrupt") {
      return {
        state: "ok",
        scope: "capture_owned_kv_payload",
        budget_bytes: budget,
        stored_bytes: null,
        reserved_bytes: null,
        records: null,
        metadata_bytes: null,
        status_records: null,
        evicted_records: null,
        evicted_bytes: null,
        expired_records: null,
        last_eviction_at_ms: null,
        last_warning_at_ms: null,
        over_budget: false,
        near_capacity: false,
        accounting_complete: false,
        accounting_error: "ledger_corrupt",
        skipped_reason: SENTINEL_REPLAY_ACCOUNTING_REASON,
      };
    }
    const ledger = withBudget(state.ledger, budget);
    const payloadBudget = sentinelReplayPayloadBudgetBytes(budget);
    const used = ledger.stored_bytes + ledger.reserved_bytes;
    return {
      state: "ok",
      scope: "capture_owned_kv_payload",
      budget_bytes: budget,
      stored_bytes: ledger.stored_bytes,
      reserved_bytes: ledger.reserved_bytes,
      records: ledger.records,
      metadata_bytes: ledger.metadata_bytes,
      status_records: ledger.status_records,
      evicted_records: ledger.evicted_records,
      evicted_bytes: ledger.evicted_bytes,
      expired_records: ledger.expired_records,
      last_eviction_at_ms: ledger.last_eviction_at_ms,
      last_warning_at_ms: ledger.last_warning_at_ms,
      over_budget: used > payloadBudget,
      near_capacity: used >= Math.floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO),
      accounting_complete: ledger.bootstrap_complete && ledger.accounting_error === null,
      accounting_error: ledger.accounting_error,
      skipped_reason: ledger.last_skip_reason,
    };
  } catch {
    return {
      state: "unavailable",
      scope: "capture_owned_kv_payload",
      budget_bytes: null,
      stored_bytes: null,
      reserved_bytes: null,
      records: null,
      metadata_bytes: null,
      status_records: null,
      evicted_records: null,
      evicted_bytes: null,
      expired_records: null,
      last_eviction_at_ms: null,
      last_warning_at_ms: null,
      over_budget: false,
      near_capacity: false,
      accounting_complete: false,
      accounting_error: "unavailable",
      skipped_reason: null,
    };
  }
};
