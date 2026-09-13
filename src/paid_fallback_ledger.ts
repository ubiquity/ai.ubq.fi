import { MICROCREDITS_PER_CREDIT, PAID_FALLBACK_NO_LIMIT } from "./api_keys.ts";
import { getKv } from "./kv.ts";
import type { PaidFallbackProvider, PaidFallbackProviderUsageV3, PaidFallbackRequestV3, PaidFallbackWindowV3 } from "./types.ts";
import { isRecord } from "./utils.ts";
import { fetchMeteredTokenLogs, type MeteredTokenLogEntry } from "./metered.ts";
import {
  isPaidFallbackUsageRollup,
  mergePaidFallbackUsageRollup,
  PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS,
  PAID_FALLBACK_USAGE_ROLLUP_PREFIX,
  type PaidFallbackUsageRollup,
  paidFallbackUsageRollupKey,
  paidFallbackUsageRollupShard,
} from "./paid_fallback_rollups.ts";

const PREFIX = ["uos_ai", "paid_fallback", "v3"] as const;
const MAX_CAS_ATTEMPTS = 128;
const RETRY_DELAYS_MS = [5_000, 30_000, 120_000, 600_000, 3_600_000, 21_600_000] as const;
const UNRESOLVED_AFTER_MS = 24 * 60 * 60_000;
const RECONCILIATION_LEASE_MS = 60_000;
const PAID_FALLBACK_BACKFILL_LEASE_MS = 5 * 60_000;
const PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT = 5_000;

/**
 * Raw paid-fallback request rows are retained for one year from request
 * creation, then expire. Research that needs older data reads the compact
 * per-hour usage rollups (paid_fallback_rollups.ts), which never expire.
 */
export const PAID_FALLBACK_REQUEST_LOG_RETENTION_MS = 365 * 24 * 60 * 60_000;

export const requestRowExpireIn = (row: PaidFallbackRequestV3, nowMs: number): number =>
  Math.max(1, row.created_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS - nowMs);

const isSafeUsageCount = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isPositiveSafeInteger = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const isApiKeyId = (value: unknown): value is string => typeof value === "string" && value.length > 0 && value.length <= 200;

/**
 * Window rows expire one year after their reset so they never outlive the
 * request rows that validate them; the per-request detail is gone by then.
 */
const windowExpireIn = (window: PaidFallbackWindowV3, nowMs: number): number =>
  Math.max(1, window.window_reset_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS - nowMs);

export const isPaidFallbackWindowV3 = (value: unknown): value is PaidFallbackWindowV3 => {
  if (!isRecord(value)) return false;
  return (
    value.v === 3 &&
    isApiKeyId(value.key_id) &&
    typeof value.policy_version === "string" &&
    value.policy_version.length > 0 &&
    isPositiveSafeInteger(value.window_reset_at_ms) &&
    (value.limit_microcredits === -1 || isPositiveSafeInteger(value.limit_microcredits)) &&
    isSafeUsageCount(value.settled_microcredits) &&
    isSafeUsageCount(value.reserved_microcredits) &&
    isSafeUsageCount(value.pending_count) &&
    isPositiveSafeInteger(value.updated_at_ms)
  );
};

export const paidFallbackWindowV3Key = (keyId: string, resetAtMs: number): Deno.KvKey => [...PREFIX, "window", keyId, resetAtMs];
export const paidFallbackRequestV3Key = (keyId: string, requestId: string): Deno.KvKey => [...PREFIX, "request", keyId, requestId];
export const paidFallbackPendingV3Key = (keyId: string, requestId: string): Deno.KvKey => [...PREFIX, "pending", keyId, requestId];
const paidFallbackWindowV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "window", keyId];
const paidFallbackRequestV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "request", keyId];
export const paidFallbackRequestV3GlobalPrefix: Deno.KvKey = [...PREFIX, "request"];
const paidFallbackWindowV3GlobalPrefix: Deno.KvKey = [...PREFIX, "window"];
const paidFallbackPendingV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "pending", keyId];
const paidFallbackPendingV3GlobalPrefix: Deno.KvKey = [...PREFIX, "pending"];
export const paidFallbackReconciliationGateV3Key = (): Deno.KvKey => [...PREFIX, "reconciliation_gate"];
export const paidFallbackReconciliationLeaseV3Key = (keyId: string): Deno.KvKey => [...PREFIX, "reconciliation_lease", keyId];
export const paidFallbackBackfillCursorV3Key = (): Deno.KvKey => [...PREFIX, "rollup_backfill_cursor"];
export const paidFallbackBackfillWindowCursorV3Key = (): Deno.KvKey => [...PREFIX, "rollup_backfill_window_cursor"];
export const paidFallbackBackfillLeaseV3Key = (): Deno.KvKey => [...PREFIX, "rollup_backfill_lease"];
export const paidFallbackBackfillStateV3Key = (): Deno.KvKey => [...PREFIX, "rollup_backfill_state"];
export const paidFallbackDeletionGuardV3Key = (keyId: string): Deno.KvKey => [...PREFIX, "deletion_guard", keyId];

type PaidFallbackPendingV3 = Readonly<{
  created_at_ms: number;
  next_reconciliation_at_ms: number;
}>;

export type PaidFallbackReconciliationGateV3 = Readonly<{
  next_due_at_ms: number | null;
}>;

type PaidFallbackReconciliationJobV3 = Readonly<{
  key_id: string;
}>;

type PaidFallbackReconciliationLeaseV3 = Readonly<{
  token: string;
  expires_at_ms: number;
}>;

type PaidFallbackBackfillLeaseV3 = Readonly<{
  token: string;
  expires_at_ms: number;
}>;

type PaidFallbackBackfillStateV3 = Readonly<{
  v: 1;
  completed_at_ms: number;
}>;

type PaidFallbackDeletionGuardV3 = Readonly<{
  created_at_ms: number;
}>;

export type PaidFallbackAdmissionV3 = Readonly<{
  key_id: string;
  request_id: string;
  created_at_ms: number;
  reserved_microcredits: number;
  quota_per_credit: number;
  window_reset_at_ms: number;
  quota_used_percent: number | null;
}>;

export type PaidFallbackWindowProjectionV3 = Readonly<{
  key_id: string;
  policy_version: string | null;
  window_reset_at_ms: number;
  limit_microcredits: number;
  settled_microcredits: number;
  reserved_microcredits: number;
  pending_count: number;
  updated_at_ms: number | null;
}>;

export type PaidFallbackOutstandingV3 = Readonly<{
  pending_requests: number;
  unresolved_requests: number;
  pending_markers: number;
  has_outstanding: boolean;
}>;

type AdmissionInput = Readonly<{
  keyId: string;
  requestId: string;
  createdAtMs: number;
  policyVersion: string;
  limitMicrocredits: number;
  maximumExposureMicrocredits: number | null;
  initialSettledMicrocredits: number;
  quotaPerCredit: number;
  windowResetAtMs: number;
  model: string;
  route: string;
  path: string;
  stream: boolean;
  reasoning: string | null;
  dispatchIntent?: boolean;
  // Present for real gateway admissions. It fences the immutable API-key
  // policy snapshot that authorized paid exposure; direct ledger tests may
  // omit it because they do not represent an API-key admission path.
  policyCheck?: Readonly<{ key: Deno.KvKey; versionstamp: string | null }>;
}>;

const resolveKv = async (kvOverride: Deno.Kv | null | undefined): Promise<Deno.Kv | null> => (kvOverride === undefined ? await getKv() : kvOverride);

const isPaidFallbackReconciliationGate = (value: unknown): value is PaidFallbackReconciliationGateV3 =>
  value !== null &&
  typeof value === "object" &&
  "next_due_at_ms" in value &&
  (value.next_due_at_ms === null || (typeof value.next_due_at_ms === "number" && Number.isSafeInteger(value.next_due_at_ms) && value.next_due_at_ms >= 0));

const paidFallbackReconciliationGateDueNow = (entry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3>, now: number): PaidFallbackReconciliationGateV3 => {
  const current = isPaidFallbackReconciliationGate(entry.value) ? entry.value.next_due_at_ms : null;
  return { next_due_at_ms: current === null ? now : Math.min(current, now) };
};

const paidFallbackReconciliationGateNeedsArm = (entry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3>, now: number): boolean => {
  if (!isPaidFallbackReconciliationGate(entry.value)) return true;
  return entry.value.next_due_at_ms === null || entry.value.next_due_at_ms > now;
};

type PendingMarkerScan = Readonly<{
  entries: readonly Deno.KvEntry<PaidFallbackPendingV3>[];
  earliest_due_at_ms: number | null;
}>;

const scanPaidFallbackPendingMarkers = async (kv: Deno.Kv): Promise<PendingMarkerScan> => {
  const entries: Deno.KvEntry<PaidFallbackPendingV3>[] = [];
  let earliestDueAtMs: number | null = null;
  for await (const entry of kv.list<PaidFallbackPendingV3>({ prefix: paidFallbackPendingV3GlobalPrefix }, { consistency: "strong" })) {
    entries.push(entry);
    const dueAtMs = entry.value.next_reconciliation_at_ms;
    if (!Number.isSafeInteger(dueAtMs) || dueAtMs < 0) continue;
    if (earliestDueAtMs === null || dueAtMs < earliestDueAtMs) earliestDueAtMs = dueAtMs;
  }
  return { entries, earliest_due_at_ms: earliestDueAtMs };
};

export const recomputePaidFallbackReconciliationGateV3 = async (kv: Deno.Kv): Promise<number | null> => {
  const gateKey = paidFallbackReconciliationGateV3Key();
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
    const scan = await scanPaidFallbackPendingMarkers(kv);
    const commit = await kv
      .atomic()
      .check(gateEntry)
      .set(gateKey, { next_due_at_ms: scan.earliest_due_at_ms } satisfies PaidFallbackReconciliationGateV3)
      .commit();
    if (commit.ok) return scan.earliest_due_at_ms;
  }
  throw new Error("Paid fallback reconciliation gate changed concurrently.");
};

const queueOptions = {
  delay: 0,
  backoffSchedule: [5_000, 30_000, 120_000, 600_000, 3_600_000, 21_600_000],
} as const;

const enqueuePaidFallbackReconciliationJob = async (kv: Deno.Kv, keyId: string, delay = 0): Promise<boolean> => {
  const enqueue = (
    kv as unknown as {
      enqueue?: (message: PaidFallbackReconciliationJobV3, options?: unknown) => Promise<unknown>;
    }
  ).enqueue;
  if (typeof enqueue !== "function") return false;
  try {
    await enqueue.call(kv, { key_id: keyId }, { ...queueOptions, delay });
    return true;
  } catch {
    // The pending marker and its backoff are durable. Cron can enqueue the key
    // again when a transient queue outage has recovered.
    return false;
  }
};

export const listPaidFallbackRequestsV3 = async (keyId: string, limit = 100, kvOverride?: Deno.Kv | null): Promise<readonly PaidFallbackRequestV3[]> => {
  const kv = await resolveKv(kvOverride);
  if (!kv || !Number.isFinite(limit) || limit <= 0) return [];
  const requests: PaidFallbackRequestV3[] = [];
  for await (const entry of kv.list<PaidFallbackRequestV3>({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })) {
    requests.push(entry.value);
  }
  requests.sort((left, right) => right.created_at_ms - left.created_at_ms || right.request_id.localeCompare(left.request_id));
  return requests.slice(0, Math.min(1_000, Math.trunc(limit)));
};

const emptyPaidFallbackProviderUsage = (): PaidFallbackProviderUsageV3 => ({
  request_count: 0,
  input_tokens: 0,
  output_tokens: 0,
  total_tokens: 0,
  spend_microcredits: 0,
});

/**
 * Returns current-window usage split by the actual paid provider. The shared
 * admission window remains provider-neutral; this projection is only an
 * operator view and does not affect quota decisions.
 */
export const getPaidFallbackProviderUsageV3 = async (
  keyId: string,
  windowResetAtMs: number,
  kvOverride?: Deno.Kv | null
): Promise<Readonly<Record<PaidFallbackProvider, PaidFallbackProviderUsageV3>>> => {
  const kv = await resolveKv(kvOverride);
  const usage: Record<PaidFallbackProvider, PaidFallbackProviderUsageV3> = {
    metered: emptyPaidFallbackProviderUsage(),
    surplus: emptyPaidFallbackProviderUsage(),
  };
  if (!kv) return usage;
  // Request keys do not include the window reset, so scan the complete key
  // history and filter by the requested window instead of truncating at the
  // newest 1,000 rows.
  for await (const entry of kv.list<PaidFallbackRequestV3>({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })) {
    const request = entry.value;
    if (request.window_reset_at_ms !== windowResetAtMs) continue;
    const provider: PaidFallbackProvider = request.provider === "surplus" ? "surplus" : "metered";
    const current = usage[provider];
    const inputTokens = request.input_tokens ?? 0;
    const outputTokens = request.output_tokens ?? 0;
    usage[provider] = {
      request_count: current.request_count + 1,
      input_tokens: current.input_tokens + inputTokens,
      output_tokens: current.output_tokens + outputTokens,
      total_tokens: current.total_tokens + inputTokens + outputTokens,
      spend_microcredits: current.spend_microcredits + (request.billing_state === "settled" ? (request.spend_microcredits ?? 0) : 0),
    };
  }
  return usage;
};

export const getPaidFallbackWindowProjectionV3 = async (
  keyId: string,
  windowResetAtMs: number,
  limitMicrocredits: number,
  kvOverride?: Deno.Kv | null
): Promise<PaidFallbackWindowProjectionV3 | null> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return null;
  const window = await kv.get<PaidFallbackWindowV3>(paidFallbackWindowV3Key(keyId, windowResetAtMs), { consistency: "strong" });
  if (window.value) {
    return {
      key_id: keyId,
      policy_version: window.value.policy_version,
      window_reset_at_ms: windowResetAtMs,
      limit_microcredits: limitMicrocredits,
      settled_microcredits: window.value.settled_microcredits,
      reserved_microcredits: window.value.reserved_microcredits,
      pending_count: window.value.pending_count,
      updated_at_ms: window.value.updated_at_ms,
    };
  }

  let policyVersion: string | null = null;
  let settledMicrocredits = 0;
  let reservedMicrocredits = 0;
  let pendingCount = 0;
  let updatedAtMs: number | null = null;
  for await (const entry of kv.list<PaidFallbackRequestV3>({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })) {
    const request = entry.value;
    if (request.window_reset_at_ms !== windowResetAtMs) continue;
    if (request.billing_state === "settled") {
      settledMicrocredits += request.spend_microcredits ?? 0;
    } else if (request.billing_state === "pending" || request.billing_state === "unresolved") {
      reservedMicrocredits += request.reserved_microcredits;
      pendingCount += 1;
    }
    if (updatedAtMs === null || request.updated_at_ms > updatedAtMs) {
      updatedAtMs = request.updated_at_ms;
      policyVersion = request.policy_version;
    }
  }
  return {
    key_id: keyId,
    policy_version: policyVersion,
    window_reset_at_ms: windowResetAtMs,
    limit_microcredits: limitMicrocredits,
    settled_microcredits: settledMicrocredits,
    reserved_microcredits: reservedMicrocredits,
    pending_count: pendingCount,
    updated_at_ms: updatedAtMs,
  };
};

export const getPaidFallbackOutstandingV3 = async (keyId: string, kvOverride?: Deno.Kv | null): Promise<PaidFallbackOutstandingV3 | null> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return null;
  let pendingRequests = 0;
  let unresolvedRequests = 0;
  for await (const entry of kv.list<PaidFallbackRequestV3>({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })) {
    if (entry.value.billing_state === "pending") pendingRequests += 1;
    if (entry.value.billing_state === "unresolved") unresolvedRequests += 1;
  }
  const pendingMarkerKeys: Deno.KvKey[] = [];
  for await (const entry of kv.list<PaidFallbackPendingV3>({ prefix: paidFallbackPendingV3Prefix(keyId) }, { consistency: "strong" })) {
    pendingMarkerKeys.push(entry.key);
  }
  const pendingMarkers = pendingMarkerKeys.length;
  return {
    pending_requests: pendingRequests,
    unresolved_requests: unresolvedRequests,
    pending_markers: pendingMarkers,
    has_outstanding: pendingRequests > 0 || unresolvedRequests > 0 || pendingMarkers > 0,
  };
};

export type PaidFallbackDeletionV3 =
  | Readonly<{
      kind: "deleted";
      deleted_requests: number;
      deleted_windows: number;
      deleted_pending: number;
      deleted_leases: number;
    }>
  | Readonly<{ kind: "blocked"; outstanding: PaidFallbackOutstandingV3 }>
  | Readonly<{ kind: "unavailable" }>;

const deleteKvKeys = async (kv: Deno.Kv, keys: readonly Deno.KvKey[]): Promise<void> => {
  for (let offset = 0; offset < keys.length; offset += 100) {
    let atomic = kv.atomic();
    for (const key of keys.slice(offset, offset + 100)) atomic = atomic.delete(key);
    const commit = await atomic.commit();
    if (!commit.ok) throw new Error("Paid fallback state deletion changed concurrently.");
  }
};

export const deletePaidFallbackStateV3 = async (keyId: string, kvOverride?: Deno.Kv | null): Promise<PaidFallbackDeletionV3> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return { kind: "unavailable" };
  const guardKey = paidFallbackDeletionGuardV3Key(keyId);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const guard = await kv.get<PaidFallbackDeletionGuardV3>(guardKey, { consistency: "strong" });
    if (guard.value) break;
    const commit = await kv.atomic().check(guard).set(guardKey, { created_at_ms: Date.now() }).commit();
    if (commit.ok) break;
    if (attempt === MAX_CAS_ATTEMPTS - 1) {
      throw new Error("Paid fallback deletion guard changed concurrently.");
    }
  }
  const outstanding = await getPaidFallbackOutstandingV3(keyId, kv);
  if (!outstanding) return { kind: "unavailable" };
  if (outstanding.has_outstanding) return { kind: "blocked", outstanding };

  const requestKeys: Deno.KvKey[] = [];
  const windowKeys: Deno.KvKey[] = [];
  const pendingKeys: Deno.KvKey[] = [];
  for await (const entry of kv.list({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })) {
    requestKeys.push(entry.key);
  }
  for await (const entry of kv.list({ prefix: paidFallbackWindowV3Prefix(keyId) }, { consistency: "strong" })) {
    windowKeys.push(entry.key);
  }
  for await (const entry of kv.list({ prefix: paidFallbackPendingV3Prefix(keyId) }, { consistency: "strong" })) {
    pendingKeys.push(entry.key);
  }
  const leaseKey = paidFallbackReconciliationLeaseV3Key(keyId);
  const lease = await kv.get(leaseKey, { consistency: "strong" });
  await deleteKvKeys(kv, [...requestKeys, ...windowKeys, ...pendingKeys, ...(lease.value ? [leaseKey] : [])]);
  return {
    kind: "deleted",
    deleted_requests: requestKeys.length,
    deleted_windows: windowKeys.length,
    deleted_pending: pendingKeys.length,
    deleted_leases: lease.value ? 1 : 0,
  };
};

export type PaidFallbackAdmissionBlockedReason = "limit_exceeded" | "invalid_policy" | "concurrent_update";

/** Default window row for a key that has no window yet. */
const defaultPaidFallbackWindowV3 = (input: AdmissionInput): PaidFallbackWindowV3 => ({
  v: 3,
  key_id: input.keyId,
  policy_version: input.policyVersion,
  window_reset_at_ms: input.windowResetAtMs,
  limit_microcredits: input.limitMicrocredits,
  settled_microcredits: input.initialSettledMicrocredits,
  reserved_microcredits: 0,
  pending_count: 0,
  updated_at_ms: input.createdAtMs,
});

/** The row written for one admitted request. */
const buildPaidFallbackRequestV3 = (input: AdmissionInput, reservation: number, now: number): PaidFallbackRequestV3 => ({
  v: 3,
  key_id: input.keyId,
  request_id: input.requestId,
  policy_version: input.policyVersion,
  route: input.route,
  path: input.path,
  model: input.model,
  stream: input.stream,
  reasoning: input.reasoning,
  window_reset_at_ms: input.windowResetAtMs,
  reserved_microcredits: reservation,
  quota_per_credit: input.quotaPerCredit,
  provider_request_id: null,
  provider_quota: null,
  input_tokens: null,
  cached_input_tokens: null,
  output_tokens: null,
  dispatch_state: input.dispatchIntent ? "dispatched" : "reserved",
  terminal_state: "pending",
  spend_microcredits: null,
  billing_state: "pending",
  reconciliation_attempts: 0,
  last_reconciliation_at_ms: null,
  dispatched_at_ms: input.dispatchIntent ? now : null,
  terminal_at_ms: null,
  settled_at_ms: null,
  created_at_ms: input.createdAtMs,
  updated_at_ms: now,
});

/** Everything the write phase of one admission attempt needs. */
type AdmissionWriteContext = Readonly<{
  unlimited: boolean;
  requestKey: Deno.KvKey;
  pendingKey: Deno.KvKey;
  windowKey: Deno.KvKey;
  gateKey: Deno.KvKey;
  requestEntry: Deno.KvEntryMaybe<PaidFallbackRequestV3>;
  windowEntry: Deno.KvEntryMaybe<PaidFallbackWindowV3>;
  deletionGuardEntry: Deno.KvEntryMaybe<PaidFallbackDeletionGuardV3>;
  transitioned: PaidFallbackWindowV3;
  reservation: number;
  now: number;
}>;

type AdmissionDecision =
  | Readonly<{ kind: "reserved"; reservation: PaidFallbackAdmissionV3 }>
  | Readonly<{ kind: "blocked"; reason: Exclude<PaidFallbackAdmissionBlockedReason, "concurrent_update"> }>
  | Readonly<{ kind: "write"; context: AdmissionWriteContext }>;

/**
 * Commits the policy transition that unblocks a limit-exceeded window so the
 * next attempt re-reads the new policy. Returns `true` when the caller should
 * report `limit_exceeded`, `false` when the CAS lost and the attempt retries.
 */
const commitExhaustedPolicyTransitionV3 = async (
  kv: Deno.Kv,
  input: AdmissionInput,
  windowKey: Deno.KvKey,
  windowEntry: Deno.KvEntryMaybe<PaidFallbackWindowV3>,
  deletionGuardEntry: Deno.KvEntryMaybe<PaidFallbackDeletionGuardV3>,
  transitioned: PaidFallbackWindowV3,
  now: number
): Promise<boolean> => {
  let atomic = kv.atomic().check(windowEntry).check(deletionGuardEntry);
  if (input.policyCheck) atomic = atomic.check(input.policyCheck);
  const transition = await atomic.set(windowKey, transitioned, { expireIn: windowExpireIn(transitioned, now) }).commit();
  return transition.ok;
};

/** Outcome for a window with no remaining exposure: blocked, or a lost CAS to retry. */
const exhaustedAdmissionV3 = async (
  kv: Deno.Kv,
  input: AdmissionInput,
  windowKey: Deno.KvKey,
  windowEntry: Deno.KvEntryMaybe<PaidFallbackWindowV3>,
  deletionGuardEntry: Deno.KvEntryMaybe<PaidFallbackDeletionGuardV3>,
  transitioned: PaidFallbackWindowV3,
  policyChanged: boolean,
  now: number
): Promise<AdmissionDecision | null> => {
  if (!policyChanged) return { kind: "blocked", reason: "limit_exceeded" };
  const transitionedPolicy = await commitExhaustedPolicyTransitionV3(kv, input, windowKey, windowEntry, deletionGuardEntry, transitioned, now);
  return transitionedPolicy ? { kind: "blocked", reason: "limit_exceeded" } : null;
};

/** Reads the current rows and decides what this admission attempt should do. */
const decidePaidFallbackAdmissionV3 = async (
  kv: Deno.Kv,
  input: AdmissionInput,
  unlimited: boolean,
  maximumExposure: number | null
): Promise<AdmissionDecision | null> => {
  const requestKey = paidFallbackRequestV3Key(input.keyId, input.requestId);
  const pendingKey = paidFallbackPendingV3Key(input.keyId, input.requestId);
  const windowKey = paidFallbackWindowV3Key(input.keyId, input.windowResetAtMs);
  const gateKey = paidFallbackReconciliationGateV3Key();

  const [requestEntry, windowEntry, deletionGuardEntry] = await Promise.all([
    kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
    unlimited
      ? Promise.resolve({ key: windowKey, value: null, versionstamp: null } as Deno.KvEntryMaybe<PaidFallbackWindowV3>)
      : kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" }),
    kv.get<PaidFallbackDeletionGuardV3>(paidFallbackDeletionGuardV3Key(input.keyId), {
      consistency: "strong",
    }),
  ]);
  if (deletionGuardEntry.value) return { kind: "blocked", reason: "invalid_policy" };
  const existing = requestEntry.value;
  if (existing) return { kind: "reserved", reservation: existingReservationV3(input, existing) };
  const current: PaidFallbackWindowV3 = windowEntry.value ?? defaultPaidFallbackWindowV3(input);
  const now = Date.now();
  const policyChanged = !unlimited && (current.policy_version !== input.policyVersion || current.limit_microcredits !== input.limitMicrocredits);
  const transitioned = policyChanged
    ? {
        ...current,
        policy_version: input.policyVersion,
        limit_microcredits: input.limitMicrocredits,
        updated_at_ms: now,
      }
    : current;
  const remaining = unlimited ? 0 : transitioned.limit_microcredits - transitioned.settled_microcredits - transitioned.reserved_microcredits;
  if (!unlimited && remaining <= 0) {
    return await exhaustedAdmissionV3(kv, input, windowKey, windowEntry, deletionGuardEntry, transitioned, policyChanged, now);
  }
  return {
    kind: "write",
    context: {
      unlimited,
      requestKey,
      pendingKey,
      windowKey,
      gateKey,
      requestEntry,
      windowEntry,
      deletionGuardEntry,
      transitioned,
      reservation: unlimited ? 0 : Math.min(remaining, maximumExposure ?? 0),
      now,
    },
  };
};

/** Writes one reservation; returns the admission, or `null` when the CAS lost. */
const writePaidFallbackAdmissionV3 = async (kv: Deno.Kv, input: AdmissionInput, context: AdmissionWriteContext): Promise<PaidFallbackAdmissionV3 | null> => {
  const { unlimited, requestKey, pendingKey, windowKey, gateKey, requestEntry, windowEntry, deletionGuardEntry, transitioned, reservation, now } = context;
  const request = buildPaidFallbackRequestV3(input, reservation, now);
  const gateEntry = input.dispatchIntent ? await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" }) : null;
  let atomic = kv.atomic().check(requestEntry).check(deletionGuardEntry);
  if (input.policyCheck) atomic = atomic.check(input.policyCheck);
  atomic = atomic.set(requestKey, request, { expireIn: requestRowExpireIn(request, now) }).set(pendingKey, {
    created_at_ms: now,
    next_reconciliation_at_ms: now,
  } satisfies PaidFallbackPendingV3);
  if (gateEntry) {
    // A dispatch-intent admission creates a new billable marker. Always
    // advance the gate version with it so a concurrent recompute cannot
    // publish a scan that predates this marker.
    atomic = atomic.check(gateEntry).set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
  }
  if (!unlimited) {
    atomic = atomic.check(windowEntry).set(
      windowKey,
      {
        ...transitioned,
        reserved_microcredits: transitioned.reserved_microcredits + reservation,
        pending_count: transitioned.pending_count + 1,
        updated_at_ms: now,
      },
      { expireIn: windowExpireIn(transitioned, now) }
    );
  }
  const commit = await atomic.commit();
  if (!commit.ok) return null;
  return {
    key_id: input.keyId,
    request_id: input.requestId,
    created_at_ms: input.createdAtMs,
    reserved_microcredits: reservation,
    quota_per_credit: input.quotaPerCredit,
    window_reset_at_ms: input.windowResetAtMs,
    quota_used_percent: unlimited ? null : (100 * transitioned.settled_microcredits) / transitioned.limit_microcredits,
  };
};

/** Projection of an already-admitted request row back into a reservation. */
const existingReservationV3 = (input: AdmissionInput, existing: PaidFallbackRequestV3): PaidFallbackAdmissionV3 => ({
  key_id: input.keyId,
  request_id: input.requestId,
  created_at_ms: existing.created_at_ms,
  reserved_microcredits: existing.reserved_microcredits,
  quota_per_credit: input.quotaPerCredit,
  window_reset_at_ms: existing.window_reset_at_ms,
  quota_used_percent: null,
});

/** True when any admission field fails its policy validation. */
const isInvalidAdmissionPolicy = (input: AdmissionInput, unlimited: boolean, maximumExposure: number | null): boolean =>
  (!unlimited && !isPositiveSafeInteger(input.limitMicrocredits)) ||
  !Number.isSafeInteger(input.initialSettledMicrocredits) ||
  input.initialSettledMicrocredits < 0 ||
  !isPositiveSafeInteger(input.quotaPerCredit) ||
  (!unlimited && !isPositiveSafeInteger(maximumExposure));

export const admitPaidFallbackV3 = async (
  input: AdmissionInput
): Promise<
  Readonly<{ kind: "reserved"; reservation: PaidFallbackAdmissionV3 }> | Readonly<{ kind: "blocked"; reason: PaidFallbackAdmissionBlockedReason }>
> => {
  const kv = await getKv();
  if (!kv) return { kind: "blocked", reason: "invalid_policy" };
  if (!paidFallbackBackfillAttempted || paidFallbackBackfillRetryPending) schedulePaidFallbackBackfill(kv);
  const unlimited = input.limitMicrocredits === PAID_FALLBACK_NO_LIMIT;
  if (isInvalidAdmissionPolicy(input, unlimited, input.maximumExposureMicrocredits)) {
    return { kind: "blocked", reason: "invalid_policy" };
  }
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const decision = await decidePaidFallbackAdmissionV3(kv, input, unlimited, input.maximumExposureMicrocredits);
    if (decision === null) continue;
    if (decision.kind !== "write") return decision;
    const reservation = await writePaidFallbackAdmissionV3(kv, input, decision.context);
    if (reservation !== null) return { kind: "reserved", reservation };
  }
  return { kind: "blocked", reason: "concurrent_update" };
};

type PaidFallbackRequestLifecyclePatchV3 = Readonly<
  Partial<
    Pick<PaidFallbackRequestV3, "provider" | "provider_request_id" | "dispatch_state" | "terminal_state"> & {
      reconciliation_attempts?: number;
      last_reconciliation_at_ms?: number | null;
      increment_reconciliation_attempts?: boolean;
    }
  >
>;

/** Lifecycle fields one patch resolves to, before they are compared to the row. */
type ResolvedRequestLifecycleV3 = Readonly<{
  provider: PaidFallbackProvider | undefined;
  providerRequestId: string | null;
  dispatchState: PaidFallbackRequestV3["dispatch_state"];
  terminalState: PaidFallbackRequestV3["terminal_state"];
  reconciliationAttempts: number;
  lastReconciliationAtMs: number | null;
}>;

/** Resolves one lifecycle patch against the stored row. */
const resolveRequestLifecyclePatchV3 = (
  current: PaidFallbackRequestV3,
  patch: PaidFallbackRequestLifecyclePatchV3,
  now: number
): ResolvedRequestLifecycleV3 => {
  const provider = current.provider ?? patch.provider;
  const providerRequestId = current.provider_request_id ?? (patch.provider_request_id === undefined ? null : patch.provider_request_id);
  const dispatchState = current.dispatch_state === "reserved" && patch.dispatch_state !== undefined ? patch.dispatch_state : current.dispatch_state;
  const terminalState = current.terminal_state === "pending" && patch.terminal_state !== undefined ? patch.terminal_state : current.terminal_state;
  const shouldIncrementReconciliationAttempts =
    patch.increment_reconciliation_attempts && (patch.terminal_state === undefined || current.terminal_state === "pending");
  const reconciliationAttempts = shouldIncrementReconciliationAttempts
    ? current.reconciliation_attempts + 1
    : (patch.reconciliation_attempts ?? current.reconciliation_attempts);
  const lastReconciliationAtMs = shouldIncrementReconciliationAttempts ? now : (patch.last_reconciliation_at_ms ?? current.last_reconciliation_at_ms);
  return { provider, providerRequestId, dispatchState, terminalState, reconciliationAttempts, lastReconciliationAtMs };
};

/** True when a resolved patch differs from the stored row. */
const requestLifecycleChangedV3 = (current: PaidFallbackRequestV3, resolved: ResolvedRequestLifecycleV3): boolean =>
  resolved.providerRequestId !== current.provider_request_id ||
  resolved.provider !== current.provider ||
  resolved.dispatchState !== current.dispatch_state ||
  resolved.terminalState !== current.terminal_state ||
  resolved.reconciliationAttempts !== current.reconciliation_attempts ||
  resolved.lastReconciliationAtMs !== current.last_reconciliation_at_ms;

/** Builds the row a resolved lifecycle patch writes. */
const buildRequestLifecycleRowV3 = (current: PaidFallbackRequestV3, resolved: ResolvedRequestLifecycleV3, now: number): PaidFallbackRequestV3 => {
  const provider = resolved.provider;
  return {
    ...current,
    ...(provider === undefined ? {} : { provider }),
    provider_request_id: resolved.providerRequestId,
    dispatch_state: resolved.dispatchState,
    terminal_state: resolved.terminalState,
    reconciliation_attempts: resolved.reconciliationAttempts,
    last_reconciliation_at_ms: resolved.lastReconciliationAtMs,
    dispatched_at_ms: resolved.dispatchState === "dispatched" && current.dispatched_at_ms === null ? now : current.dispatched_at_ms,
    terminal_at_ms: resolved.terminalState !== "pending" && current.terminal_at_ms === null ? now : current.terminal_at_ms,
    updated_at_ms: now,
  };
};

/** Reads the reconciliation gate when a lifecycle transition must arm it. */
const loadLifecycleGateEntryV3 = async (
  kv: Deno.Kv,
  gateKey: Deno.KvKey,
  shouldArm: boolean
): Promise<Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3> | null> =>
  shouldArm ? await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" }) : null;

export const updatePaidFallbackRequestV3 = async (reservation: PaidFallbackAdmissionV3, patch: PaidFallbackRequestLifecyclePatchV3): Promise<void> => {
  const kv = await getKv();
  if (!kv) return;
  const key = paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackRequestV3>(key, { consistency: "strong" });
    if (!entry.value) return;
    const current = entry.value;
    const now = Date.now();
    const resolved = resolveRequestLifecyclePatchV3(current, patch, now);
    if (!requestLifecycleChangedV3(current, resolved)) return;
    const dispatchBoundary = current.dispatch_state !== "dispatched" && (resolved.dispatchState === "dispatched" || resolved.providerRequestId !== null);
    const shouldArmReconciliationGate = dispatchBoundary || (current.provider_request_id === null && resolved.providerRequestId !== null);
    const gateKey = paidFallbackReconciliationGateV3Key();
    const gateEntry = await loadLifecycleGateEntryV3(kv, gateKey, shouldArmReconciliationGate);
    let atomic = kv
      .atomic()
      .check(entry)
      .set(key, buildRequestLifecycleRowV3(current, resolved, now), { expireIn: requestRowExpireIn(current, now) });
    if (gateEntry) {
      atomic = atomic.check(gateEntry);
      if (dispatchBoundary || paidFallbackReconciliationGateNeedsArm(gateEntry, now)) {
        atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
      }
    }
    if ((await atomic.commit()).ok) return;
  }
  throw new Error("Paid fallback request changed concurrently.");
};

const releasePaidFallbackBeforeDispatchV3 = async (reservation: PaidFallbackAdmissionV3, allowDispatchIntent: boolean): Promise<void> => {
  const kv = await getKv();
  if (!kv) return;
  const requestKey = paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
  const pendingKey = paidFallbackPendingV3Key(reservation.key_id, reservation.request_id);
  const windowKey = paidFallbackWindowV3Key(reservation.key_id, reservation.window_reset_at_ms);
  const gateKey = paidFallbackReconciliationGateV3Key();
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, windowEntry, gateEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" }),
      kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" }),
    ]);
    if (
      requestEntry.value?.billing_state !== "pending" ||
      requestEntry.value.provider_request_id !== null ||
      (requestEntry.value.dispatch_state !== "reserved" && !(allowDispatchIntent && requestEntry.value.dispatch_state === "dispatched"))
    )
      return;
    const now = Date.now();
    let atomic = kv
      .atomic()
      .check(requestEntry)
      .set(
        requestKey,
        {
          ...requestEntry.value,
          dispatch_state: "not_dispatched",
          terminal_state: "cancelled",
          spend_microcredits: 0,
          billing_state: "not_billed",
          terminal_at_ms: requestEntry.value.terminal_at_ms ?? now,
          updated_at_ms: now,
        },
        { expireIn: requestRowExpireIn(requestEntry.value, now) }
      )
      .delete(pendingKey);
    atomic = atomic.check(gateEntry);
    // Deleting a pending marker must version-bump the gate even when it is
    // already due; otherwise an overlapping recompute can publish a stale
    // future timestamp for this row.
    atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    if (windowEntry.value) {
      atomic = atomic.check(windowEntry).set(
        windowKey,
        {
          ...windowEntry.value,
          reserved_microcredits: Math.max(0, windowEntry.value.reserved_microcredits - requestEntry.value.reserved_microcredits),
          pending_count: Math.max(0, windowEntry.value.pending_count - 1),
          updated_at_ms: now,
        },
        { expireIn: windowExpireIn(windowEntry.value, now) }
      );
    }
    if ((await atomic.commit()).ok) return;
  }
  throw new Error("Paid fallback release changed concurrently.");
};

export const releaseUndispatchedPaidFallbackV3 = async (reservation: PaidFallbackAdmissionV3): Promise<void> => {
  await releasePaidFallbackBeforeDispatchV3(reservation, false);
};

/**
 * Releases a reservation after a durable dispatch intent was written but
 * before provider fetch was invoked. This must never be called once provider
 * fetch can have started.
 */
export const releasePaidFallbackBeforeProviderFetchV3 = async (reservation: PaidFallbackAdmissionV3): Promise<void> => {
  await releasePaidFallbackBeforeDispatchV3(reservation, true);
};

const acquireReconciliationLease = async (kv: Deno.Kv, keyId: string, now: number): Promise<PaidFallbackReconciliationLeaseV3 | null> => {
  const key = paidFallbackReconciliationLeaseV3Key(keyId);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackReconciliationLeaseV3>(key, { consistency: "strong" });
    if (entry.value && entry.value.expires_at_ms > now) return null;
    const lease = {
      token: crypto.randomUUID(),
      expires_at_ms: now + RECONCILIATION_LEASE_MS,
    } satisfies PaidFallbackReconciliationLeaseV3;
    const commit = await kv
      .atomic()
      .check(entry)
      .set(key, lease, {
        expireIn: RECONCILIATION_LEASE_MS,
      })
      .commit();
    if (commit.ok) return lease;
  }
  return null;
};

const releaseReconciliationLease = async (kv: Deno.Kv, keyId: string, lease: PaidFallbackReconciliationLeaseV3): Promise<void> => {
  const key = paidFallbackReconciliationLeaseV3Key(keyId);
  const entry = await kv.get<PaidFallbackReconciliationLeaseV3>(key, { consistency: "strong" });
  if (entry.value?.token !== lease.token) return;
  await kv.atomic().check(entry).delete(key).commit();
};

const _expeditePaidFallbackReconciliationV3 = async (reservation: PaidFallbackAdmissionV3, now: number): Promise<void> => {
  const kv = await getKv();
  if (!kv) return;
  const pendingKey = paidFallbackPendingV3Key(reservation.key_id, reservation.request_id);
  const gateKey = paidFallbackReconciliationGateV3Key();
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [pending, gateEntry] = await Promise.all([
      kv.get<PaidFallbackPendingV3>(pendingKey, { consistency: "strong" }),
      kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" }),
    ]);
    if (!pending.value) return;
    const markerNeedsExpedite = pending.value.next_reconciliation_at_ms > now;
    const gateNeedsArm = paidFallbackReconciliationGateNeedsArm(gateEntry, now);
    if (!markerNeedsExpedite && !gateNeedsArm) return;
    let atomic = kv.atomic().check(pending).check(gateEntry);
    if (markerNeedsExpedite) {
      atomic = atomic.set(pendingKey, {
        ...pending.value,
        next_reconciliation_at_ms: now,
      } satisfies PaidFallbackPendingV3);
    }
    // A real expedite must version-bump the gate even when it is already due;
    // otherwise an overlapping recompute can overwrite the new marker with a
    // stale future timestamp.
    atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    if ((await atomic.commit()).ok) {
      return;
    }
  }
  throw new Error("Paid fallback reconciliation scheduling changed concurrently.");
};

const deferPaidFallbackReconciliationV3 = async (kv: Deno.Kv, keyId: string, requestId: string, now: number): Promise<number | null> => {
  const requestKey = paidFallbackRequestV3Key(keyId, requestId);
  const pendingKey = paidFallbackPendingV3Key(keyId, requestId);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, pendingEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      kv.get<PaidFallbackPendingV3>(pendingKey, { consistency: "strong" }),
    ]);
    if (!pendingEntry.value) return null;
    const request = requestEntry.value;
    if (!request || request.billing_state === "settled" || request.billing_state === "not_billed") {
      const gateKey = paidFallbackReconciliationGateV3Key();
      const gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
      let cleanup = kv.atomic().check(pendingEntry).delete(pendingKey);
      cleanup = cleanup.check(gateEntry);
      cleanup = cleanup.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
      const cleanupResult = await cleanup.commit();
      if (cleanupResult.ok) return null;
      continue;
    }
    const attempts = request.reconciliation_attempts + 1;
    const unresolved = now - request.created_at_ms >= UNRESOLVED_AFTER_MS;
    const delay = RETRY_DELAYS_MS[Math.min(attempts - 1, RETRY_DELAYS_MS.length - 1)];
    const atomic = kv
      .atomic()
      .check(requestEntry)
      .check(pendingEntry)
      .set(
        requestKey,
        {
          ...request,
          reconciliation_attempts: attempts,
          last_reconciliation_at_ms: now,
          billing_state: unresolved ? "unresolved" : request.billing_state,
          updated_at_ms: now,
        },
        { expireIn: requestRowExpireIn(request, now) }
      )
      .set(pendingKey, {
        ...pendingEntry.value,
        next_reconciliation_at_ms: now + delay,
      } satisfies PaidFallbackPendingV3);
    if ((await atomic.commit()).ok) return delay;
  }
  throw new Error("Paid fallback reconciliation deferral changed concurrently.");
};

/**
 * True when this provider log belongs to the request being settled. Surplus
 * usage is delivered synchronously for its own gateway reservation, so it
 * matches on the provider field instead of a fabricated upstream request ID.
 */
const correlationMatchesV3 = (
  request: PaidFallbackRequestV3,
  providerLog: MeteredTokenLogEntry,
  correlation: "provider_request_id" | "surplus_reservation"
): boolean => (correlation === "surplus_reservation" ? request.provider === "surplus" : request.provider_request_id === providerLog.request_id);

/** True when the row still needs settlement; a settled or unbilled row does not. */
const isBillableRequestV3 = (request: PaidFallbackRequestV3 | null): request is PaidFallbackRequestV3 =>
  request !== null && request.billing_state !== "settled" && request.billing_state !== "not_billed";

/** Clears a stale pending marker for a row that can no longer be billed. */
const clearStalePendingMarkerV3 = async (
  kv: Deno.Kv,
  pendingKey: Deno.KvKey,
  pendingEntry: Deno.KvEntryMaybe<PaidFallbackPendingV3>,
  gateKey: Deno.KvKey,
  gateEntry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3>,
  now: number
): Promise<boolean> => {
  if (!pendingEntry.value) return true;
  let cleanup = kv.atomic().check(pendingEntry).delete(pendingKey);
  cleanup = cleanup.check(gateEntry);
  cleanup = cleanup.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
  return (await cleanup.commit()).ok;
};

/** The settled request row written by one settlement. */
const buildSettledRequestRowV3 = (
  request: PaidFallbackRequestV3,
  providerLog: MeteredTokenLogEntry,
  spend: number,
  dispatchedAtMs: number,
  foldIntoRollup: boolean,
  now: number
): PaidFallbackRequestV3 => ({
  ...request,
  provider_quota: providerLog.quota,
  input_tokens: providerLog.prompt_tokens,
  cached_input_tokens: providerLog.cached_prompt_tokens ?? null,
  output_tokens: providerLog.completion_tokens,
  dispatch_state: request.dispatch_state === "reserved" ? "dispatched" : request.dispatch_state,
  dispatched_at_ms: dispatchedAtMs,
  spend_microcredits: spend,
  billing_state: "settled",
  // The terminal lifecycle update records the attempt that triggered this
  // lookup. Settlement itself must be replay-idempotent and not inflate
  // the attempt count when a queue delivery is duplicated.
  reconciliation_attempts: request.reconciliation_attempts,
  last_reconciliation_at_ms: now,
  settled_at_ms: request.settled_at_ms ?? now,
  updated_at_ms: now,
  // The settlement write just folded this usage into the hourly rollup;
  // mark it so a later backfill run cannot double-count it.
  ...(foldIntoRollup ? { usage_rollup_at_ms: now } : {}),
});

/** The usage rollup one settlement folds into. */
type SettlementRollupV3 = Readonly<{
  rollupKey: Deno.KvKey;
  rollupEntry: Deno.KvEntryMaybe<PaidFallbackUsageRollup>;
  nextRollup: PaidFallbackUsageRollup;
  /** False when the model or provider label is empty, so no rollup is written. */
  foldIntoRollup: boolean;
}>;

/** Reads and merges the usage rollup this settlement belongs to. */
const prepareSettlementRollupV3 = async (
  kv: Deno.Kv,
  request: PaidFallbackRequestV3,
  providerLog: MeteredTokenLogEntry,
  spend: number,
  correlation: "provider_request_id" | "surplus_reservation",
  now: number
): Promise<SettlementRollupV3> => {
  const model = request.model.trim();
  // Typed as a plain string on purpose: the request row field is a literal
  // union, but the rollup identity below still guards against an empty label
  // so a legacy or hand-written row can never key a rollup on "".
  const provider: string = request.provider ?? (correlation === "surplus_reservation" ? "surplus" : "metered");
  const bucketStartAtMs = Math.floor(request.created_at_ms / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
  const rollupKey = paidFallbackUsageRollupKey(bucketStartAtMs, model, provider, paidFallbackUsageRollupShard(request.request_id));
  const rollupEntry = await kv.get<PaidFallbackUsageRollup>(rollupKey, { consistency: "strong" });
  const existingRollup = isPaidFallbackUsageRollup(rollupEntry.value) ? rollupEntry.value : null;
  const nextRollup = mergePaidFallbackUsageRollup(existingRollup, {
    bucket_start_at_ms: bucketStartAtMs,
    request_id: request.request_id,
    model,
    provider,
    quota: providerLog.quota,
    input_tokens: providerLog.prompt_tokens,
    cached_input_tokens: providerLog.cached_prompt_tokens ?? null,
    output_tokens: providerLog.completion_tokens,
    spend_microcredits: spend,
    request_created_at_ms: request.created_at_ms,
    updated_at_ms: now,
  });
  return { rollupKey, rollupEntry, nextRollup, foldIntoRollup: model !== "" && provider !== "" };
};

/** Applies a settlement to the window row when the window row exists. */
const applySettlementWindowUpdateV3 = (
  atomic: Deno.AtomicOperation,
  windowEntry: Deno.KvEntryMaybe<PaidFallbackWindowV3>,
  windowKey: Deno.KvKey,
  request: PaidFallbackRequestV3,
  spend: number,
  now: number
): Deno.AtomicOperation => {
  const window = windowEntry.value;
  if (!window) return atomic;
  return atomic.check(windowEntry).set(
    windowKey,
    {
      ...window,
      settled_microcredits: window.settled_microcredits + spend,
      reserved_microcredits: Math.max(0, window.reserved_microcredits - request.reserved_microcredits),
      pending_count: Math.max(0, window.pending_count - 1),
      updated_at_ms: now,
    },
    { expireIn: windowExpireIn(window, now) }
  );
};

const settlePaidFallbackRequestV3 = async (
  kv: Deno.Kv,
  keyId: string,
  requestId: string,
  providerLog: MeteredTokenLogEntry,
  now: number,
  correlation: "provider_request_id" | "surplus_reservation" = "provider_request_id"
): Promise<Readonly<{ settled: boolean; retry_delay_ms: number | null }>> => {
  const requestKey = paidFallbackRequestV3Key(keyId, requestId);
  const pendingKey = paidFallbackPendingV3Key(keyId, requestId);
  const gateKey = paidFallbackReconciliationGateV3Key();
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, pendingEntry, gateEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      kv.get<PaidFallbackPendingV3>(pendingKey, { consistency: "strong" }),
      kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" }),
    ]);
    const request = requestEntry.value;
    if (!isBillableRequestV3(request)) {
      const cleared = await clearStalePendingMarkerV3(kv, pendingKey, pendingEntry, gateKey, gateEntry, now);
      if (!cleared) continue;
      return { settled: false, retry_delay_ms: null };
    }
    // Surplus usage is delivered synchronously for this exact gateway
    // reservation. It does not need a fabricated upstream request ID; the
    // provider field prevents this direct path from settling a Metered row.
    if (!pendingEntry.value || !correlationMatchesV3(request, providerLog, correlation)) {
      return { settled: false, retry_delay_ms: null };
    }
    const calculatedSpend = Math.round((providerLog.quota * MICROCREDITS_PER_CREDIT) / request.quota_per_credit);
    if (!Number.isSafeInteger(calculatedSpend) || calculatedSpend < 0) {
      return {
        settled: false,
        retry_delay_ms: await deferPaidFallbackReconciliationV3(kv, keyId, requestId, now),
      };
    }
    // A provider can report more usage than the exposure admitted for this
    // request. Preserve the actual provider spend so the window cannot
    // under-report billed usage; the admission exposure only bounds the
    // amount we risk before the provider responds.
    const spend = calculatedSpend;
    const windowKey = paidFallbackWindowV3Key(keyId, request.window_reset_at_ms);
    const windowEntry = await kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" });
    const dispatchedAtMs = request.dispatched_at_ms ?? Math.max(request.created_at_ms, providerLog.created_at * 1_000);
    const rollup = await prepareSettlementRollupV3(kv, request, providerLog, spend, correlation, now);
    let atomic = kv
      .atomic()
      .check(requestEntry)
      .check(pendingEntry)
      .set(requestKey, buildSettledRequestRowV3(request, providerLog, spend, dispatchedAtMs, rollup.foldIntoRollup, now), {
        expireIn: requestRowExpireIn(request, now),
      })
      .delete(pendingKey);
    if (rollup.foldIntoRollup) {
      atomic = atomic.check(rollup.rollupEntry).set(rollup.rollupKey, rollup.nextRollup);
    }
    atomic = atomic.check(gateEntry);
    // Settlement removes billable work. Keep the gate due and version it so a
    // concurrent recompute cannot resurrect this marker's stale future time.
    atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    atomic = applySettlementWindowUpdateV3(atomic, windowEntry, windowKey, request, spend, now);
    if ((await atomic.commit()).ok) return { settled: true, retry_delay_ms: null };
  }
  throw new Error("Paid fallback settlement changed concurrently.");
};

export type PaidFallbackUsageSettlementV3 = Readonly<{
  /** Internal direct-settlement key; never exposed as an upstream request ID. */
  settlement_request_id: string;
  provider_quota: number;
  input_tokens: number;
  cached_input_tokens?: number | null;
  output_tokens: number;
  model: string;
  created_at_ms: number;
}>;

/**
 * Settles a provider whose response contains authoritative usage instead of
 * exposing the OpenLux token-log endpoint. The same CAS path is used as the
 * asynchronous OpenLux reconciliation, so duplicate terminal observations
 * remain idempotent.
 */
export const settlePaidFallbackUsageV3 = async (reservation: PaidFallbackAdmissionV3, usage: PaidFallbackUsageSettlementV3): Promise<boolean> => {
  const kv = await getKv();
  if (!kv) return false;
  const result = await settlePaidFallbackRequestV3(
    kv,
    reservation.key_id,
    reservation.request_id,
    {
      request_id: usage.settlement_request_id,
      quota: usage.provider_quota,
      prompt_tokens: usage.input_tokens,
      ...(usage.cached_input_tokens === null || usage.cached_input_tokens === undefined ? {} : { cached_prompt_tokens: usage.cached_input_tokens }),
      completion_tokens: usage.output_tokens,
      model: usage.model,
      created_at: Math.max(0, Math.trunc(usage.created_at_ms / 1_000)),
    },
    Date.now(),
    "surplus_reservation"
  );
  return result.settled;
};

export type PaidFallbackRollupBackfillResult = Readonly<{
  scanned: number;
  /** Rows this run applied (TTL rewrite and/or rollup merge) successfully. */
  processed: number;
  rollups_written: number;
  /**
   * Rows whose CAS commit failed (for example a live settlement updated the
   * same shard in between). The cursor is kept before them so a re-run
   * retries instead of losing the history.
   */
  failed: number;
  /** True when the scan stopped at `limit` rows and a further run is needed. */
  truncated: boolean;
}>;

/**
 * One-time backfill that folds already-settled V3 rows into usage rollups and
 * applies the one-year raw-row TTL to pre-existing rows.
 *
 * Rollups are new with this feature: rows settled before deployment never
 * passed through the settlement hook, so without this pass the admin
 * projection would silently start from an empty history. It also fixes rows
 * that predate the TTL, because every request-row write now re-applies an
 * anchored expiry and these rows will not be rewritten by normal traffic.
 *
 * Idempotency: rows carry `usage_rollup_at_ms` set by the settlement write for
 * new traffic and by this backfill for historical rows, so a run can never
 * double-count usage already folded into a rollup. Run with a `limit` and
 * re-run until `truncated` is false; a persisted cursor resumes the next run
 * at the last committed row (the list `start` is inclusive) so repeated
 * batches stay O(remaining) rather than re-walking every already-processed
 * row. The walk itself is also scan-bounded so an invocation cannot exceed
 * its deadline without persisting forward progress.
 */
/** Persists the last committed row so the next run resumes before the stalled one. */
const persistResumeBeforeRowV3 = async (kv: Deno.Kv, cursorKey: Deno.KvKey, lastVisitedKey: Deno.KvKey | null, resumeKey: Deno.KvKey | null): Promise<void> => {
  const cursorKeyValue = lastVisitedKey ?? resumeKey;
  if (cursorKeyValue !== null) await persistBackfillCursorV3(kv, cursorKey, cursorKeyValue);
};

/** Re-applies the anchored retention TTL to a row that was already folded. */
const rewriteMarkedRequestRowTtlV3 = async (
  kv: Deno.Kv,
  requestKey: Deno.KvKey,
  requestEntry: Deno.KvEntryMaybe<PaidFallbackRequestV3>,
  request: PaidFallbackRequestV3,
  nowMs: number
): Promise<boolean> => {
  try {
    const committed = await kv
      .atomic()
      .check(requestEntry)
      .set(requestKey, request, { expireIn: requestRowExpireIn(request, nowMs) })
      .commit();
    return committed.ok;
  } catch {
    return false;
  }
};

/** Outcome of folding one request row into its usage rollup. */
type RollupFoldOutcome = "processed" | "rollup_written" | "failed";

/**
 * Marks one request row as backfilled and, when it is a settled billable row,
 * folds its usage into the matching hourly rollup.
 */
const foldRequestRowIntoRollupV3 = async (
  kv: Deno.Kv,
  requestKey: Deno.KvKey,
  requestEntry: Deno.KvEntryMaybe<PaidFallbackRequestV3>,
  request: PaidFallbackRequestV3,
  nowMs: number
): Promise<RollupFoldOutcome> => {
  const settled = request.billing_state === "settled" && request.provider_quota !== null && request.spend_microcredits !== null;
  const model = request.model.trim();
  const provider = request.provider ?? "metered";
  const bucketStartAtMs = Math.floor(request.created_at_ms / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS;
  const rollupKey = paidFallbackUsageRollupKey(bucketStartAtMs, model, provider, paidFallbackUsageRollupShard(request.request_id));
  const rollupEntry = settled && model ? await kv.get<PaidFallbackUsageRollup>(rollupKey, { consistency: "strong" }) : null;
  const existingRollup = isPaidFallbackUsageRollup(rollupEntry?.value) ? rollupEntry.value : null;
  const nextRollup =
    settled && model
      ? mergePaidFallbackUsageRollup(existingRollup, {
          bucket_start_at_ms: bucketStartAtMs,
          request_id: request.request_id,
          model,
          provider,
          quota: request.provider_quota,
          input_tokens: request.input_tokens ?? 0,
          cached_input_tokens: request.cached_input_tokens ?? null,
          output_tokens: request.output_tokens ?? 0,
          spend_microcredits: request.spend_microcredits,
          request_created_at_ms: request.created_at_ms,
          updated_at_ms: nowMs,
        })
      : null;
  // Every processed row is marked, including non-billable rows, so a bounded
  // run always advances past them instead of consuming its budget on the same
  // rows forever. This is safe for pending/unresolved rows: the live
  // settlement path unconditionally folds and re-marks a row whenever it
  // actually settles, so marking one early can never suppress a rollup.
  const nextRow = { ...request, updated_at_ms: nowMs, usage_rollup_at_ms: nowMs };
  let atomic = kv
    .atomic()
    .check(requestEntry)
    .set(requestKey, nextRow, {
      expireIn: requestRowExpireIn(request, nowMs),
    });
  if (nextRollup && rollupEntry) {
    atomic = atomic.check(rollupEntry).set(rollupKey, nextRollup);
  }
  if (!(await atomic.commit()).ok) {
    // A concurrent writer touched this row or its rollup shard. Stop the
    // sweep immediately so the persisted cursor stays before this row: a
    // later success must never let the cursor skip a missing rollup.
    return "failed";
  }
  return nextRollup ? "rollup_written" : "processed";
};

/** Where the sweep stopped, and which cursor fallback that stop needs. */
type RollupBackfillTruncation = "scan_budget" | "write_budget" | null;

type RollupBackfillSweep = Readonly<{
  scanned: number;
  processed: number;
  rollupsWritten: number;
  failed: number;
  lastVisitedKey: Deno.KvKey | null;
  truncatedWith: RollupBackfillTruncation;
}>;

/** Mutable counters for one bounded sweep. */
type BackfillProgress = {
  scanned: number;
  processed: number;
  rollupsWritten: number;
  failed: number;
  budget: number;
  scanRemaining: number;
  lastVisitedKey: Deno.KvKey | null;
};

/**
 * Applies one row of the sweep to the progress counters. Returns where the
 * sweep must stop, or `"continue"` when the walk may go on.
 */
const advanceBackfillSweepV3 = async (
  kv: Deno.Kv,
  requestKey: Deno.KvKey,
  progress: BackfillProgress,
  nowMs: number
): Promise<"continue" | RollupBackfillTruncation> => {
  const requestEntry = await kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" });
  const request = requestEntry.value;
  if (!request) return "continue";
  if (typeof request.usage_rollup_at_ms === "number") {
    // A previously processed row can lose its TTL (for example after a KV
    // export/import restores values without expiry). Re-apply the anchored
    // retention without touching the rollup or the row value; marked rows
    // never consume the run budget.
    const rewritten = await rewriteMarkedRequestRowTtlV3(kv, requestKey, requestEntry, request, nowMs);
    if (!rewritten) {
      progress.failed += 1;
      return "write_budget";
    }
    progress.lastVisitedKey = requestKey;
    return "continue";
  }
  if (progress.budget <= 0) return "write_budget";
  progress.budget -= 1;
  const outcome = await foldRequestRowIntoRollupV3(kv, requestKey, requestEntry, request, nowMs);
  if (outcome === "failed") {
    progress.failed += 1;
    return "write_budget";
  }
  progress.lastVisitedKey = requestKey;
  progress.processed += 1;
  if (outcome === "rollup_written") progress.rollupsWritten += 1;
  return "continue";
};

/** One bounded sweep of the request-row prefix. */
const sweepPaidFallbackUsageRollupsV3 = async (
  kv: Deno.Kv,
  resumeKey: Deno.KvKey | null,
  limit: number,
  scanBudget: number,
  nowMs: number
): Promise<RollupBackfillSweep> => {
  const progress: BackfillProgress = { scanned: 0, processed: 0, rollupsWritten: 0, failed: 0, budget: limit, scanRemaining: scanBudget, lastVisitedKey: null };
  const selector: Deno.KvListSelector =
    resumeKey === null ? { prefix: paidFallbackRequestV3GlobalPrefix } : { prefix: paidFallbackRequestV3GlobalPrefix, start: resumeKey };
  for await (const entry of kv.list<PaidFallbackRequestV3>(selector)) {
    progress.scanned += 1;
    if (progress.scanRemaining <= 0) return toRollupBackfillSweepV3(progress, "scan_budget");
    progress.scanRemaining -= 1;
    const step = await advanceBackfillSweepV3(kv, entry.key, progress, nowMs);
    if (step === "continue") continue;
    return toRollupBackfillSweepV3(progress, step);
  }
  return toRollupBackfillSweepV3(progress, progress.failed > 0 ? "write_budget" : null);
};

/** Freezes the sweep counters into a result. */
const toRollupBackfillSweepV3 = (progress: BackfillProgress, truncatedWith: RollupBackfillTruncation): RollupBackfillSweep => ({
  scanned: progress.scanned,
  processed: progress.processed,
  rollupsWritten: progress.rollupsWritten,
  failed: progress.failed,
  lastVisitedKey: progress.lastVisitedKey,
  truncatedWith,
});

/**
 * One-time backfill that folds already-settled V3 rows into usage rollups and
 * applies the one-year raw-row TTL to pre-existing rows.
 *
 * Rollups are new with this feature: rows settled before deployment never
 * passed through the settlement hook, so without this pass the admin
 * projection would silently start from an empty history. It also fixes rows
 * that predate the TTL, because every request-row write now re-applies an
 * anchored expiry and these rows will not be rewritten by normal traffic.
 *
 * Idempotency: rows carry `usage_rollup_at_ms` set by the settlement write for
 * new traffic and by this backfill for historical rows, so a run can never
 * double-count usage already folded into a rollup. Run with a `limit` and
 * re-run until `truncated` is false; a persisted cursor resumes the next run
 * at the last committed row (the list `start` is inclusive) so repeated
 * batches stay O(remaining) rather than re-walking every already-processed
 * row. The walk itself is also scan-bounded so an invocation cannot exceed
 * its deadline without persisting forward progress.
 */
export const backfillPaidFallbackUsageRollups = async (
  kv: Deno.Kv,
  options: Readonly<{ limit?: number; nowMs?: number }> = {}
): Promise<PaidFallbackRollupBackfillResult> => {
  const limit = Math.max(1, Math.min(10_000, Math.trunc(options.limit ?? 5_000)));
  const nowMs = Math.trunc(options.nowMs ?? Date.now());
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new Error("Paid fallback backfill clock is invalid");
  // The budget bounds mutating work; the scan budget bounds the walk itself
  // (marked rows skip the write budget but still cost a get and a TTL rewrite)
  // so a resumed run cannot exceed its deadline without advancing the cursor.
  const cursorKey = paidFallbackBackfillCursorV3Key();
  const resumeKey = await loadBackfillCursorV3(kv, cursorKey);
  const sweep = await sweepPaidFallbackUsageRollupsV3(kv, resumeKey, limit, limit * 10, nowMs);
  const result: PaidFallbackRollupBackfillResult = {
    scanned: sweep.scanned,
    processed: sweep.processed,
    rollups_written: sweep.rollupsWritten,
    failed: sweep.failed,
    truncated: sweep.truncatedWith !== null,
  };
  if (sweep.truncatedWith === null) {
    // Full sweep complete: drop the resume cursor.
    await kv
      .atomic()
      .delete(cursorKey)
      .commit()
      .catch(() => {});
    return result;
  }
  if (sweep.truncatedWith === "scan_budget") {
    // Persist the resume cursor: `start` is inclusive, so the next run
    // restarts at this row instead of re-walking the whole prefix.
    if (sweep.lastVisitedKey !== null) await persistBackfillCursorV3(kv, cursorKey, sweep.lastVisitedKey);
    return result;
  }
  // Budget exhaustion or a failed row: keep the cursor at the last committed
  // row so the next run resumes before the row that still needs work.
  await persistResumeBeforeRowV3(kv, cursorKey, sweep.lastVisitedKey, resumeKey);
  return result;
};

export type PaidFallbackWindowTtlBackfillResult = Readonly<{
  scanned: number;
  rewritten: number;
  truncated: boolean;
}>;

/** True when both KV keys hold the same parts, in order. */
const isSameKvKey = (left: Deno.KvKey, right: Deno.KvKey): boolean => left.length === right.length && left.every((part, index) => part === right[index]);

/** Reads a persisted resume cursor; `null` when absent or malformed. */
const loadBackfillCursorV3 = async (kv: Deno.Kv, cursorKey: Deno.KvKey): Promise<Deno.KvKey | null> => {
  const cursorEntry = await kv.get<{ request_key: unknown }>(cursorKey).catch(() => null);
  const requestKey = cursorEntry?.value?.request_key;
  return Array.isArray(requestKey) ? (requestKey as Deno.KvKey) : null;
};

/** Persists a resume cursor; a KV outage just leaves the sweep to restart. */
const persistBackfillCursorV3 = async (kv: Deno.Kv, cursorKey: Deno.KvKey, requestKey: Deno.KvKey): Promise<void> => {
  await kv
    .atomic()
    .set(cursorKey, { request_key: requestKey })
    .commit()
    .catch(() => {});
};

/** Outcome of rewriting one window row's TTL. */
type WindowTtlRewrite = "skipped" | "rewritten" | "failed";

/** Rewrites one window row with its anchored TTL. */
const rewriteWindowTtlV3 = async (kv: Deno.Kv, key: Deno.KvKey, nowMs: number): Promise<WindowTtlRewrite> => {
  const windowEntry = await kv.get<PaidFallbackWindowV3>(key, { consistency: "strong" });
  if (!isPaidFallbackWindowV3(windowEntry.value)) return "skipped";
  try {
    const committed = await kv
      .atomic()
      .check(windowEntry)
      .set(key, windowEntry.value, { expireIn: windowExpireIn(windowEntry.value, nowMs) })
      .commit();
    return committed.ok ? "rewritten" : "failed";
  } catch {
    return "failed";
  }
};

/** Reads at most one entry matching a selector; `null` when nothing matches. */
const readFirstKvEntry = async <TValue>(kv: Deno.Kv, selector: Deno.KvListSelector): Promise<Deno.KvEntry<TValue> | null> => {
  for await (const entry of kv.list<TValue>(selector, { consistency: "strong" })) {
    return entry;
  }
  return null;
};

/** True when the window prefix still holds rows after `startKey` itself. */
const hasWindowRowsAfterV3 = async (kv: Deno.Kv, startKey: Deno.KvKey): Promise<boolean> => {
  const seenKeys: Deno.KvKey[] = [];
  for await (const entry of kv.list({ prefix: paidFallbackWindowV3GlobalPrefix, start: startKey })) {
    seenKeys.push(entry.key);
    if (seenKeys.length > 1) return true;
  }
  return false;
};

type WindowTtlSweep = Readonly<{
  scanned: number;
  rewritten: number;
  lastRewrittenKey: Deno.KvKey | null;
  /** A row lost its CAS or threw; the caller keeps the cursor before it. */
  failed: boolean;
  /** The batch limit was reached and more rows remain; the cursor is persisted. */
  truncated: boolean;
}>;

/** One bounded window-TTL sweep. */
const sweepWindowTtlsV3 = async (kv: Deno.Kv, cursorKey: Deno.KvKey, resumeKey: Deno.KvKey | null, limit: number, nowMs: number): Promise<WindowTtlSweep> => {
  let scanned = 0;
  let rewritten = 0;
  let lastRewrittenKey: Deno.KvKey | null = null;
  const selector: Deno.KvListSelector =
    resumeKey === null ? { prefix: paidFallbackWindowV3GlobalPrefix } : { prefix: paidFallbackWindowV3GlobalPrefix, start: resumeKey };
  let skippingResumeRow = resumeKey !== null;
  for await (const entry of kv.list<PaidFallbackWindowV3>(selector)) {
    scanned += 1;
    // `start` is inclusive; skip the cursor row itself so a resumed run is
    // not forced to rewrite it and then trip the batch limit again.
    const resumeCursor = resumeKey;
    if (skippingResumeRow && resumeCursor !== null && isSameKvKey(entry.key, resumeCursor)) {
      skippingResumeRow = false;
      continue;
    }
    const outcome = await rewriteWindowTtlV3(kv, entry.key, nowMs);
    if (outcome === "skipped") continue;
    if (outcome === "failed") return { scanned, rewritten, lastRewrittenKey, failed: true, truncated: false };
    lastRewrittenKey = entry.key;
    rewritten += 1;
    if (rewritten < limit) continue;
    // Probe for rows beyond this batch before reporting truncation, so the
    // final batch is never reported as incomplete.
    const hasMore = await hasWindowRowsAfterV3(kv, entry.key);
    if (!hasMore) break;
    await persistBackfillCursorV3(kv, cursorKey, entry.key);
    return { scanned, rewritten, lastRewrittenKey, failed: false, truncated: true };
  }
  return { scanned, rewritten, lastRewrittenKey, failed: false, truncated: false };
};

/**
 * Resumable sweep that applies the anchored one-year window TTL to existing
 * window rows. Window writes in live paths already carry the expiry; rows
 * written before this feature or restored by KV import do not, so they must
 * be rewritten in place. The rewrite is idempotent and each run bounds the
 * work, persisting its own cursor so repeated invocations complete the whole
 * prefix.
 */
export const backfillPaidFallbackWindowTtls = async (
  kv: Deno.Kv,
  options: Readonly<{ limit?: number; nowMs?: number }> = {}
): Promise<PaidFallbackWindowTtlBackfillResult> => {
  const limit = Math.max(1, Math.min(10_000, Math.trunc(options.limit ?? 5_000)));
  const nowMs = Math.trunc(options.nowMs ?? Date.now());
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) throw new Error("Paid fallback window TTL clock is invalid");
  const cursorKey = paidFallbackBackfillWindowCursorV3Key();
  const resumeKey = await loadBackfillCursorV3(kv, cursorKey);
  const sweep = await sweepWindowTtlsV3(kv, cursorKey, resumeKey, limit, nowMs);
  if (sweep.failed) {
    // Keep the cursor at the last successful rewrite so the next run retries
    // the row that lost its CAS instead of advancing past its missing TTL.
    const cursorKeyValue = sweep.lastRewrittenKey ?? resumeKey;
    if (cursorKeyValue !== null) await persistBackfillCursorV3(kv, cursorKey, cursorKeyValue);
    return { scanned: sweep.scanned, rewritten: sweep.rewritten, truncated: true };
  }
  if (sweep.truncated) {
    // The sweep already persisted the resume cursor for this batch.
    return { scanned: sweep.scanned, rewritten: sweep.rewritten, truncated: true };
  }
  await kv
    .atomic()
    .delete(cursorKey)
    .commit()
    .catch(() => {});
  return { scanned: sweep.scanned, rewritten: sweep.rewritten, truncated: false };
};

export type PaidFallbackAutomaticBackfillResult = Readonly<{
  kind: "completed" | "in_progress" | "skipped" | "busy";
  requests: PaidFallbackRollupBackfillResult | null;
  windows: PaidFallbackWindowTtlBackfillResult | null;
}>;

const isPaidFallbackBackfillState = (value: unknown): value is PaidFallbackBackfillStateV3 =>
  isRecord(value) && value.v === 1 && typeof value.completed_at_ms === "number" && Number.isSafeInteger(value.completed_at_ms) && value.completed_at_ms >= 0;

const isBackfillableSettledPaidFallbackRequest = (value: unknown): boolean => {
  if (!isRecord(value)) return false;
  return (
    value.billing_state === "settled" &&
    typeof value.model === "string" &&
    value.model.trim().length > 0 &&
    typeof value.provider_quota === "number" &&
    Number.isFinite(value.provider_quota) &&
    value.provider_quota >= 0 &&
    typeof value.spend_microcredits === "number" &&
    Number.isFinite(value.spend_microcredits) &&
    value.spend_microcredits >= 0
  );
};

const hasPaidFallbackUsageRollups = async (kv: Deno.Kv): Promise<boolean> => {
  const firstRollup = await readFirstKvEntry(kv, { prefix: PAID_FALLBACK_USAGE_ROLLUP_PREFIX });
  return firstRollup !== null;
};

const hasBackfillableSettledPaidFallbackRequest = async (kv: Deno.Kv): Promise<boolean> => {
  for await (const entry of kv.list<PaidFallbackRequestV3>({ prefix: paidFallbackRequestV3GlobalPrefix }, { consistency: "strong" })) {
    if (isBackfillableSettledPaidFallbackRequest(entry.value)) return true;
  }
  return false;
};

const paidFallbackBackfillNeedsRun = async (kv: Deno.Kv): Promise<boolean> => {
  const [stateEntry, requestCursorEntry, windowCursorEntry] = await Promise.all([
    kv.get<PaidFallbackBackfillStateV3>(paidFallbackBackfillStateV3Key(), { consistency: "strong" }),
    kv.get(paidFallbackBackfillCursorV3Key(), { consistency: "strong" }),
    kv.get(paidFallbackBackfillWindowCursorV3Key(), { consistency: "strong" }),
  ]);
  if (!isPaidFallbackBackfillState(stateEntry.value) || requestCursorEntry.value !== null || windowCursorEntry.value !== null) {
    return true;
  }
  if (await hasPaidFallbackUsageRollups(kv)) return false;
  return await hasBackfillableSettledPaidFallbackRequest(kv);
};

const acquirePaidFallbackBackfillLease = async (kv: Deno.Kv, nowMs: number): Promise<PaidFallbackBackfillLeaseV3 | null> => {
  const leaseKey = paidFallbackBackfillLeaseV3Key();
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackBackfillLeaseV3>(leaseKey, { consistency: "strong" });
    if (entry.value && entry.value.expires_at_ms > nowMs) return null;
    const lease = {
      token: crypto.randomUUID(),
      expires_at_ms: nowMs + PAID_FALLBACK_BACKFILL_LEASE_MS,
    } satisfies PaidFallbackBackfillLeaseV3;
    const commit = await kv
      .atomic()
      .check(entry)
      .set(leaseKey, lease, {
        expireIn: PAID_FALLBACK_BACKFILL_LEASE_MS,
      })
      .commit();
    if (commit.ok) return lease;
  }
  return null;
};

const releasePaidFallbackBackfillLease = async (kv: Deno.Kv, lease: PaidFallbackBackfillLeaseV3): Promise<void> => {
  try {
    const entry = await kv.get<PaidFallbackBackfillLeaseV3>(paidFallbackBackfillLeaseV3Key(), { consistency: "strong" });
    if (entry.value?.token !== lease.token) return;
    await kv
      .atomic()
      .check(entry)
      .delete(paidFallbackBackfillLeaseV3Key())
      .commit()
      .catch(() => {});
  } catch {
    // Lease expiry is the recovery path when a release races a KV outage.
  }
};

/**
 * Executes the request-row and window-TTL sweeps under one durable lease.
 * Cursor keys let a bootstrap pass resume on a later request or revision,
 * while the completion marker avoids paying the scan cost on every request.
 */
/**
 * Surfaces a failed state write. A bare `set` has no CAS precondition, so
 * `Deno.KvCommitResult.ok` is typed as the literal `true`; the check is kept
 * because the completion marker must be durable before it is reported.
 */
const throwIfCommitFailed = (result: { readonly ok: boolean }, message: string): void => {
  if (!result.ok) throw new Error(message);
};

export const runPaidFallbackBackfillV3 = async (
  kv: Deno.Kv,
  options: Readonly<{ force?: boolean; limit?: number; nowMs?: number }> = {}
): Promise<PaidFallbackAutomaticBackfillResult> => {
  const requestedLimit = Math.trunc(options.limit ?? PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT);
  const limit = Number.isFinite(requestedLimit) ? Math.max(1, Math.min(10_000, requestedLimit)) : PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT;
  const nowMs = Math.trunc(options.nowMs ?? Date.now());
  if (!Number.isSafeInteger(nowMs) || nowMs < 0) {
    throw new Error("Paid fallback automatic backfill clock is invalid");
  }
  if (options.force !== true && !(await paidFallbackBackfillNeedsRun(kv))) {
    return { kind: "skipped", requests: null, windows: null };
  }
  const lease = await acquirePaidFallbackBackfillLease(kv, nowMs);
  if (!lease) return { kind: "busy", requests: null, windows: null };
  try {
    const requests = await backfillPaidFallbackUsageRollups(kv, { limit, nowMs });
    const windows = await backfillPaidFallbackWindowTtls(kv, { limit, nowMs });
    const complete = !requests.truncated && !windows.truncated;
    if (complete) {
      throwIfCommitFailed(
        await kv.set(paidFallbackBackfillStateV3Key(), {
          v: 1,
          completed_at_ms: nowMs,
        } satisfies PaidFallbackBackfillStateV3),
        "Paid fallback automatic backfill state changed concurrently."
      );
    }
    return {
      kind: complete ? "completed" : "in_progress",
      requests,
      windows,
    };
  } finally {
    await releasePaidFallbackBackfillLease(kv, lease);
  }
};

let scheduledPaidFallbackBackfill: Promise<void> | null = null;
let paidFallbackBackfillAttempted = false;
let paidFallbackBackfillRetryPending = false;

const schedulePaidFallbackBackfill = (kvOverride?: Deno.Kv): void => {
  if (scheduledPaidFallbackBackfill) return;
  paidFallbackBackfillAttempted = true;
  scheduledPaidFallbackBackfill = (kvOverride ? Promise.resolve(kvOverride) : resolveKv(undefined))
    .then(async (kv) => {
      if (!kv) {
        paidFallbackBackfillRetryPending = true;
        return;
      }
      const result = await runPaidFallbackBackfillV3(kv);
      paidFallbackBackfillRetryPending = result.kind === "in_progress" || result.kind === "busy";
    })
    .catch(() => {
      paidFallbackBackfillRetryPending = true;
    })
    .finally(() => {
      scheduledPaidFallbackBackfill = null;
    });
};

/** Clears due pending markers whose request row is gone or no longer billable. */
const collectReconcileCandidatesV3 = async (
  kv: Deno.Kv,
  keyId: string,
  due: readonly Deno.KvEntry<PaidFallbackPendingV3>[],
  now: number
): Promise<Deno.KvEntry<PaidFallbackRequestV3>[]> => {
  const candidates: Deno.KvEntry<PaidFallbackRequestV3>[] = [];
  for (const pending of due) {
    const requestId = String(pending.key.at(-1));
    const requestKey = paidFallbackRequestV3Key(keyId, requestId);
    const requestEntry = await kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" });
    const request = requestEntry.value;
    if (!request || request.billing_state === "settled" || request.billing_state === "not_billed") {
      const gateKey = paidFallbackReconciliationGateV3Key();
      const gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
      await kv.atomic().check(pending).check(gateEntry).delete(pending.key).set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now)).commit();
      continue;
    }
    candidates.push(requestEntry as Deno.KvEntry<PaidFallbackRequestV3>);
  }
  return candidates;
};

/** Fetches the Metered token log for these candidates; `null` when the fetch failed. */
const loadMeteredLogsForCandidatesV3 = async (
  candidates: readonly Deno.KvEntry<PaidFallbackRequestV3>[],
  now: number
): Promise<readonly MeteredTokenLogEntry[] | null> => {
  const providerRequestIds = candidates
    .map((requestEntry) => requestEntry.value.provider_request_id)
    .filter((requestId): requestId is string => requestId !== null);
  if (providerRequestIds.length === 0) return [];
  try {
    return await fetchMeteredTokenLogs({
      requestIds: providerRequestIds,
      startAtMs: Math.min(...candidates.map((requestEntry) => requestEntry.value.created_at_ms)) - 60_000,
      endAtMs: now + 60_000,
    });
  } catch {
    return null;
  }
};

/** Settles every candidate that has an authoritative Metered log entry. */
const settleMeteredCandidatesV3 = async (
  kv: Deno.Kv,
  keyId: string,
  candidates: readonly Deno.KvEntry<PaidFallbackRequestV3>[],
  byId: ReadonlyMap<string, MeteredTokenLogEntry>,
  now: number
): Promise<number> => {
  let settled = 0;
  for (const requestEntry of candidates) {
    const request = requestEntry.value;
    const providerLog = request.provider_request_id ? byId.get(request.provider_request_id) : null;
    if (!providerLog) {
      await deferPaidFallbackReconciliationV3(kv, keyId, request.request_id, now);
      continue;
    }
    const result = await settlePaidFallbackRequestV3(kv, keyId, request.request_id, providerLog, now);
    if (result.settled) settled += 1;
  }
  return settled;
};

export const reconcilePaidFallbackV3 = async (
  keyId: string,
  now = Date.now(),
  kvOverride?: Deno.Kv | null,
  options?: Readonly<{ skipGateRecompute?: boolean }>
): Promise<number> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  const lease = await acquireReconciliationLease(kv, keyId, now);
  if (!lease) return 0;
  try {
    const due: Deno.KvEntry<PaidFallbackPendingV3>[] = [];
    for await (const pending of kv.list<PaidFallbackPendingV3>({ prefix: paidFallbackPendingV3Prefix(keyId) })) {
      if (pending.value.next_reconciliation_at_ms <= now) due.push(pending);
    }
    if (!due.length) return 0;

    const candidates = await collectReconcileCandidatesV3(kv, keyId, due, now);
    if (!candidates.length) return 0;
    // Surplus has no Metered token-log endpoint. Its terminal usage is settled
    // synchronously by recordSurplusUsage; a missing or partial observation
    // must remain pending and fail closed rather than being looked up or
    // falsely settled through Metered logs.
    const surplusCandidates = candidates.filter((requestEntry) => requestEntry.value.provider === "surplus");
    await Promise.all(surplusCandidates.map((requestEntry) => deferPaidFallbackReconciliationV3(kv, keyId, requestEntry.value.request_id, now)));
    const meteredCandidates = candidates.filter((requestEntry) => requestEntry.value.provider !== "surplus");
    if (!meteredCandidates.length) return 0;
    const logs = await loadMeteredLogsForCandidatesV3(meteredCandidates, now);
    if (logs === null) {
      await Promise.all(meteredCandidates.map((requestEntry) => deferPaidFallbackReconciliationV3(kv, keyId, requestEntry.value.request_id, now)));
      // The durable marker carries the retry timestamp. New Deno Deploy
      // reconciles it from cron because KV queue delivery is unavailable.
      return 0;
    }
    // The durable marker carries the retry timestamp. New Deno Deploy
    // reconciles it from cron because KV queue delivery is unavailable.
    return await settleMeteredCandidatesV3(kv, keyId, meteredCandidates, new Map(logs.map((log) => [log.request_id, log])), now);
  } finally {
    await releaseReconciliationLease(kv, keyId, lease);
    if (!options?.skipGateRecompute) await recomputePaidFallbackReconciliationGateV3(kv);
  }
};

export const reconcileDuePaidFallbacksV3 = async (now = Date.now(), kvOverride?: Deno.Kv | null): Promise<number> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  const gateKey = paidFallbackReconciliationGateV3Key();
  let gateEntry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3> | null = null;
  let bootstrapScan: PendingMarkerScan | null = null;
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
    if (isPaidFallbackReconciliationGate(gateEntry.value)) break;
    bootstrapScan = await scanPaidFallbackPendingMarkers(kv);
    const commit = await kv
      .atomic()
      .check(gateEntry)
      .set(gateKey, { next_due_at_ms: bootstrapScan.earliest_due_at_ms } satisfies PaidFallbackReconciliationGateV3)
      .commit();
    if (commit.ok) {
      gateEntry = {
        key: gateKey,
        value: { next_due_at_ms: bootstrapScan.earliest_due_at_ms },
        versionstamp: commit.versionstamp,
      };
      break;
    }
    bootstrapScan = null;
  }
  if (!gateEntry || !isPaidFallbackReconciliationGate(gateEntry.value)) {
    throw new Error("Paid fallback reconciliation gate changed concurrently.");
  }
  const nextDueAtMs = gateEntry.value.next_due_at_ms;
  if (nextDueAtMs === null || nextDueAtMs > now) return 0;

  const pendingScan = bootstrapScan ?? (await scanPaidFallbackPendingMarkers(kv));
  const keyIds = new Set<string>();
  for (const pending of pendingScan.entries) {
    if (pending.value.next_reconciliation_at_ms > now) continue;
    const keyId = pending.key.at(-2);
    if (typeof keyId === "string") keyIds.add(keyId);
  }
  let settled = 0;
  try {
    for (const keyId of keyIds) {
      settled += await reconcilePaidFallbackV3(keyId, now, kv, { skipGateRecompute: true });
    }
    return settled;
  } finally {
    await recomputePaidFallbackReconciliationGateV3(kv);
  }
};

export const markPaidFallbackTerminalV3 = async (
  reservation: PaidFallbackAdmissionV3,
  terminalState: PaidFallbackRequestV3["terminal_state"],
  provider?: PaidFallbackProvider
): Promise<number> => {
  await updatePaidFallbackRequestV3(reservation, {
    ...(provider === undefined ? {} : { provider }),
    terminal_state: terminalState,
    increment_reconciliation_attempts: true,
  });
  // A terminal event can arrive while the pending marker is still scheduled
  // for a later retry. Move that marker to "due" before queueing so the
  // consumer never burns a delivery on a no-op reconciliation.
  await _expeditePaidFallbackReconciliationV3(reservation, Date.now());
  // The queue consumer owns provider-log reads and settlement. Never fetch
  // provider logs from the inference request or an admin read.
  return 0;
};

export const recordPaidFallbackTerminalV3 = async (
  reservation: PaidFallbackAdmissionV3,
  terminalState: PaidFallbackRequestV3["terminal_state"],
  provider?: PaidFallbackProvider
): Promise<number> => {
  await markPaidFallbackTerminalV3(reservation, terminalState, provider);
  // Legacy direct callers are retained only for deterministic local migration
  // tests; production inference calls markPaidFallbackTerminalV3 instead.
  return await reconcilePaidFallbackV3(reservation.key_id, Date.now());
};

export const handlePaidFallbackReconciliationJobV3 = async (message: unknown, kvOverride?: Deno.Kv | null): Promise<number> => {
  if (!message || typeof message !== "object") return 0;
  const keyId = "key_id" in message && typeof message.key_id === "string" ? message.key_id : null;
  if (!keyId) return 0;
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  return await reconcilePaidFallbackV3(keyId, Date.now(), kv);
};

export const enqueueDuePaidFallbackReconciliationJobsV3 = async (now = Date.now(), kvOverride?: Deno.Kv | null): Promise<number> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  const keyIds = new Set<string>();
  for await (const pending of kv.list<PaidFallbackPendingV3>({ prefix: paidFallbackPendingV3GlobalPrefix })) {
    if (pending.value.next_reconciliation_at_ms > now) continue;
    const keyId = pending.key.at(-2);
    if (typeof keyId === "string") keyIds.add(keyId);
  }
  let queued = 0;
  for (const keyId of keyIds) {
    if (await enqueuePaidFallbackReconciliationJob(kv, keyId)) queued += 1;
  }
  return queued;
};
