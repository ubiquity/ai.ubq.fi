import { MICROCREDITS_PER_CREDIT, PAID_FALLBACK_NO_LIMIT } from "./api_keys.ts";
import { getKv } from "./kv.ts";
import type {
  PaidFallbackProvider,
  PaidFallbackProviderUsageV3,
  PaidFallbackRequestV3,
  PaidFallbackWindowV3,
} from "./types.ts";
import { fetchMeteredTokenLogs, type MeteredTokenLogEntry } from "./metered.ts";

const PREFIX = ["uos_ai", "paid_fallback", "v3"] as const;
const MAX_CAS_ATTEMPTS = 128;
const RETRY_DELAYS_MS = [5_000, 30_000, 120_000, 600_000, 3_600_000, 21_600_000] as const;
const UNRESOLVED_AFTER_MS = 24 * 60 * 60_000;
const RECONCILIATION_LEASE_MS = 60_000;

export const paidFallbackWindowV3Key = (keyId: string, resetAtMs: number): Deno.KvKey => [
  ...PREFIX,
  "window",
  keyId,
  resetAtMs,
];
export const paidFallbackRequestV3Key = (keyId: string, requestId: string): Deno.KvKey => [
  ...PREFIX,
  "request",
  keyId,
  requestId,
];
export const paidFallbackPendingV3Key = (keyId: string, requestId: string): Deno.KvKey => [
  ...PREFIX,
  "pending",
  keyId,
  requestId,
];
const paidFallbackWindowV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "window", keyId];
const paidFallbackRequestV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "request", keyId];
const paidFallbackPendingV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "pending", keyId];
const paidFallbackPendingV3GlobalPrefix: Deno.KvKey = [...PREFIX, "pending"];
export const paidFallbackReconciliationGateV3Key = (): Deno.KvKey => [
  ...PREFIX,
  "reconciliation_gate",
];
export const paidFallbackReconciliationLeaseV3Key = (keyId: string): Deno.KvKey => [
  ...PREFIX,
  "reconciliation_lease",
  keyId,
];
export const paidFallbackDeletionGuardV3Key = (keyId: string): Deno.KvKey => [
  ...PREFIX,
  "deletion_guard",
  keyId,
];

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

const resolveKv = async (kvOverride: Deno.Kv | null | undefined): Promise<Deno.Kv | null> =>
  kvOverride === undefined ? await getKv() : kvOverride;

const isPaidFallbackReconciliationGate = (
  value: unknown,
): value is PaidFallbackReconciliationGateV3 =>
  value !== null && typeof value === "object" &&
  "next_due_at_ms" in value &&
  (value.next_due_at_ms === null ||
    (typeof value.next_due_at_ms === "number" && Number.isSafeInteger(value.next_due_at_ms) &&
      value.next_due_at_ms >= 0));

const paidFallbackReconciliationGateDueNow = (
  entry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3>,
  now: number,
): PaidFallbackReconciliationGateV3 => {
  const current = isPaidFallbackReconciliationGate(entry.value) ? entry.value.next_due_at_ms : null;
  return { next_due_at_ms: current === null ? now : Math.min(current, now) };
};

const paidFallbackReconciliationGateNeedsArm = (
  entry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3>,
  now: number,
): boolean => {
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
  for await (
    const entry of kv.list<PaidFallbackPendingV3>(
      { prefix: paidFallbackPendingV3GlobalPrefix },
      { consistency: "strong" },
    )
  ) {
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
    const commit = await kv.atomic()
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

const enqueuePaidFallbackReconciliationJob = async (
  kv: Deno.Kv,
  keyId: string,
  delay = 0,
): Promise<boolean> => {
  const enqueue = (kv as unknown as {
    enqueue?: (message: PaidFallbackReconciliationJobV3, options?: unknown) => Promise<unknown>;
  }).enqueue;
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

export const listPaidFallbackRequestsV3 = async (
  keyId: string,
  limit = 100,
  kvOverride?: Deno.Kv | null,
): Promise<readonly PaidFallbackRequestV3[]> => {
  const kv = await resolveKv(kvOverride);
  if (!kv || !Number.isFinite(limit) || limit <= 0) return [];
  const requests: PaidFallbackRequestV3[] = [];
  for await (
    const entry of kv.list<PaidFallbackRequestV3>(
      { prefix: paidFallbackRequestV3Prefix(keyId) },
      { consistency: "strong" },
    )
  ) {
    requests.push(entry.value);
  }
  requests.sort((left, right) =>
    right.created_at_ms - left.created_at_ms || right.request_id.localeCompare(left.request_id)
  );
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
  kvOverride?: Deno.Kv | null,
): Promise<Readonly<Record<PaidFallbackProvider, PaidFallbackProviderUsageV3>>> => {
  const usage: Record<PaidFallbackProvider, PaidFallbackProviderUsageV3> = {
    metered: emptyPaidFallbackProviderUsage(),
    surplus: emptyPaidFallbackProviderUsage(),
  };
  const requests = await listPaidFallbackRequestsV3(keyId, 1_000, kvOverride);
  for (const request of requests) {
    if (request.window_reset_at_ms !== windowResetAtMs) continue;
    const provider = request.provider ?? "metered";
    const current = usage[provider];
    const inputTokens = request.input_tokens ?? 0;
    const outputTokens = request.output_tokens ?? 0;
    usage[provider] = {
      request_count: current.request_count + 1,
      input_tokens: current.input_tokens + inputTokens,
      output_tokens: current.output_tokens + outputTokens,
      total_tokens: current.total_tokens + inputTokens + outputTokens,
      spend_microcredits: current.spend_microcredits +
        (request.billing_state === "settled" ? request.spend_microcredits ?? 0 : 0),
    };
  }
  return usage;
};

export const getPaidFallbackWindowProjectionV3 = async (
  keyId: string,
  windowResetAtMs: number,
  limitMicrocredits: number,
  kvOverride?: Deno.Kv | null,
): Promise<PaidFallbackWindowProjectionV3 | null> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return null;
  const window = await kv.get<PaidFallbackWindowV3>(
    paidFallbackWindowV3Key(keyId, windowResetAtMs),
    { consistency: "strong" },
  );
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
  for await (
    const entry of kv.list<PaidFallbackRequestV3>(
      { prefix: paidFallbackRequestV3Prefix(keyId) },
      { consistency: "strong" },
    )
  ) {
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

export const getPaidFallbackOutstandingV3 = async (
  keyId: string,
  kvOverride?: Deno.Kv | null,
): Promise<PaidFallbackOutstandingV3 | null> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return null;
  let pendingRequests = 0;
  let unresolvedRequests = 0;
  let pendingMarkers = 0;
  for await (
    const entry of kv.list<PaidFallbackRequestV3>(
      { prefix: paidFallbackRequestV3Prefix(keyId) },
      { consistency: "strong" },
    )
  ) {
    if (entry.value.billing_state === "pending") pendingRequests += 1;
    if (entry.value.billing_state === "unresolved") unresolvedRequests += 1;
  }
  for await (
    const _entry of kv.list<PaidFallbackPendingV3>(
      { prefix: paidFallbackPendingV3Prefix(keyId) },
      { consistency: "strong" },
    )
  ) {
    pendingMarkers += 1;
  }
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

export const deletePaidFallbackStateV3 = async (
  keyId: string,
  kvOverride?: Deno.Kv | null,
): Promise<PaidFallbackDeletionV3> => {
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
  for await (
    const entry of kv.list({ prefix: paidFallbackRequestV3Prefix(keyId) }, { consistency: "strong" })
  ) {
    requestKeys.push(entry.key);
  }
  for await (
    const entry of kv.list({ prefix: paidFallbackWindowV3Prefix(keyId) }, { consistency: "strong" })
  ) {
    windowKeys.push(entry.key);
  }
  for await (
    const entry of kv.list({ prefix: paidFallbackPendingV3Prefix(keyId) }, { consistency: "strong" })
  ) {
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

export const admitPaidFallbackV3 = async (
  input: AdmissionInput,
): Promise<
  | Readonly<{ kind: "reserved"; reservation: PaidFallbackAdmissionV3 }>
  | Readonly<{ kind: "blocked"; reason: "limit_exceeded" | "invalid_policy" | "concurrent_update" }>
> => {
  const kv = await getKv();
  if (!kv) return { kind: "blocked", reason: "invalid_policy" };
  const unlimited = input.limitMicrocredits === PAID_FALLBACK_NO_LIMIT;
  if (
    (!unlimited && (!Number.isSafeInteger(input.limitMicrocredits) || input.limitMicrocredits <= 0)) ||
    !Number.isSafeInteger(input.initialSettledMicrocredits) ||
    input.initialSettledMicrocredits < 0 ||
    !Number.isSafeInteger(input.quotaPerCredit) ||
    input.quotaPerCredit <= 0 ||
    !unlimited &&
      (!Number.isSafeInteger(input.maximumExposureMicrocredits) || input.maximumExposureMicrocredits! <= 0)
  ) {
    return { kind: "blocked", reason: "invalid_policy" };
  }
  const requestKey = paidFallbackRequestV3Key(input.keyId, input.requestId);
  const pendingKey = paidFallbackPendingV3Key(input.keyId, input.requestId);
  const windowKey = paidFallbackWindowV3Key(input.keyId, input.windowResetAtMs);
  const gateKey = paidFallbackReconciliationGateV3Key();

  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, windowEntry, deletionGuardEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      unlimited
        ? Promise.resolve(
          { key: windowKey, value: null, versionstamp: null } as Deno.KvEntryMaybe<PaidFallbackWindowV3>,
        )
        : kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" }),
      kv.get<PaidFallbackDeletionGuardV3>(paidFallbackDeletionGuardV3Key(input.keyId), {
        consistency: "strong",
      }),
    ]);
    if (deletionGuardEntry.value) return { kind: "blocked", reason: "invalid_policy" };
    if (requestEntry.value) {
      return {
        kind: "reserved",
        reservation: {
          key_id: input.keyId,
          request_id: input.requestId,
          created_at_ms: requestEntry.value.created_at_ms,
          reserved_microcredits: requestEntry.value.reserved_microcredits,
          quota_per_credit: input.quotaPerCredit,
          window_reset_at_ms: requestEntry.value.window_reset_at_ms,
          quota_used_percent: null,
        },
      };
    }
    const current: PaidFallbackWindowV3 = windowEntry.value ?? {
      v: 3,
      key_id: input.keyId,
      policy_version: input.policyVersion,
      window_reset_at_ms: input.windowResetAtMs,
      limit_microcredits: input.limitMicrocredits,
      settled_microcredits: input.initialSettledMicrocredits,
      reserved_microcredits: 0,
      pending_count: 0,
      updated_at_ms: input.createdAtMs,
    };
    const now = Date.now();
    const policyChanged = !unlimited &&
      (current.policy_version !== input.policyVersion || current.limit_microcredits !== input.limitMicrocredits);
    const transitioned = policyChanged
      ? {
        ...current,
        policy_version: input.policyVersion,
        limit_microcredits: input.limitMicrocredits,
        updated_at_ms: now,
      }
      : current;
    const remaining = unlimited
      ? 0
      : transitioned.limit_microcredits - transitioned.settled_microcredits - transitioned.reserved_microcredits;
    if (!unlimited && remaining <= 0) {
      if (!policyChanged) return { kind: "blocked", reason: "limit_exceeded" };
      let atomic = kv.atomic().check(windowEntry).check(deletionGuardEntry);
      if (input.policyCheck) atomic = atomic.check(input.policyCheck);
      const transition = await atomic.set(windowKey, transitioned).commit();
      if (transition.ok) return { kind: "blocked", reason: "limit_exceeded" };
      continue;
    }
    const reservation = unlimited ? 0 : Math.min(remaining, input.maximumExposureMicrocredits!);
    const request: PaidFallbackRequestV3 = {
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
    };
    const gateEntry = input.dispatchIntent
      ? await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" })
      : null;
    let atomic = kv.atomic().check(requestEntry).check(deletionGuardEntry);
    if (input.policyCheck) atomic = atomic.check(input.policyCheck);
    atomic = atomic.set(requestKey, request).set(
      pendingKey,
      {
        created_at_ms: now,
        next_reconciliation_at_ms: now,
      } satisfies PaidFallbackPendingV3,
    );
    if (gateEntry) {
      // A dispatch-intent admission creates a new billable marker. Always
      // advance the gate version with it so a concurrent recompute cannot
      // publish a scan that predates this marker.
      atomic = atomic.check(gateEntry).set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    }
    if (!unlimited) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...transitioned,
        reserved_microcredits: transitioned.reserved_microcredits + reservation,
        pending_count: transitioned.pending_count + 1,
        updated_at_ms: now,
      });
    }
    const commit = await atomic.commit();
    if (commit.ok) {
      return {
        kind: "reserved",
        reservation: {
          key_id: input.keyId,
          request_id: input.requestId,
          created_at_ms: input.createdAtMs,
          reserved_microcredits: reservation,
          quota_per_credit: input.quotaPerCredit,
          window_reset_at_ms: input.windowResetAtMs,
          quota_used_percent: unlimited
            ? null
            : 100 * transitioned.settled_microcredits / transitioned.limit_microcredits,
        },
      };
    }
  }
  return { kind: "blocked", reason: "concurrent_update" };
};

type PaidFallbackRequestLifecyclePatchV3 = Readonly<
  Partial<
    Pick<
      PaidFallbackRequestV3,
      "provider" | "provider_request_id" | "dispatch_state" | "terminal_state"
    > & {
      reconciliation_attempts?: number;
      last_reconciliation_at_ms?: number | null;
      increment_reconciliation_attempts?: boolean;
    }
  >
>;

export const updatePaidFallbackRequestV3 = async (
  reservation: PaidFallbackAdmissionV3,
  patch: PaidFallbackRequestLifecyclePatchV3,
): Promise<void> => {
  const kv = await getKv();
  if (!kv) return;
  const key = paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackRequestV3>(key, { consistency: "strong" });
    if (!entry.value) return;
    const current = entry.value;
    const now = Date.now();
    const provider = current.provider ?? patch.provider;
    const providerRequestId = current.provider_request_id ??
      (patch.provider_request_id === undefined ? null : patch.provider_request_id);
    const dispatchState = current.dispatch_state === "reserved" && patch.dispatch_state !== undefined
      ? patch.dispatch_state
      : current.dispatch_state;
    const terminalState = current.terminal_state === "pending" && patch.terminal_state !== undefined
      ? patch.terminal_state
      : current.terminal_state;
    const shouldIncrementReconciliationAttempts = patch.increment_reconciliation_attempts &&
      (patch.terminal_state === undefined || current.terminal_state === "pending");
    const reconciliationAttempts = shouldIncrementReconciliationAttempts
      ? current.reconciliation_attempts + 1
      : patch.reconciliation_attempts ?? current.reconciliation_attempts;
    const lastReconciliationAtMs = shouldIncrementReconciliationAttempts
      ? now
      : patch.last_reconciliation_at_ms ?? current.last_reconciliation_at_ms;
    if (
      providerRequestId === current.provider_request_id &&
      provider === current.provider &&
      dispatchState === current.dispatch_state &&
      terminalState === current.terminal_state &&
      reconciliationAttempts === current.reconciliation_attempts &&
      lastReconciliationAtMs === current.last_reconciliation_at_ms
    ) return;
    const dispatchBoundary = current.dispatch_state !== "dispatched" &&
      (dispatchState === "dispatched" || providerRequestId !== null);
    const shouldArmReconciliationGate = dispatchBoundary ||
      (current.provider_request_id === null && providerRequestId !== null);
    const gateKey = paidFallbackReconciliationGateV3Key();
    const gateEntry = shouldArmReconciliationGate
      ? await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" })
      : null;
    let atomic = kv.atomic().check(entry).set(key, {
      ...current,
      ...(provider === undefined ? {} : { provider }),
      provider_request_id: providerRequestId,
      dispatch_state: dispatchState,
      terminal_state: terminalState,
      reconciliation_attempts: reconciliationAttempts,
      last_reconciliation_at_ms: lastReconciliationAtMs,
      dispatched_at_ms: dispatchState === "dispatched" && current.dispatched_at_ms === null
        ? now
        : current.dispatched_at_ms,
      terminal_at_ms: terminalState !== "pending" && current.terminal_at_ms === null ? now : current.terminal_at_ms,
      updated_at_ms: now,
    });
    if (gateEntry) {
      atomic = atomic.check(gateEntry);
      if (dispatchBoundary || paidFallbackReconciliationGateNeedsArm(gateEntry, now)) {
        atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
      }
    }
    const commit = await atomic.commit();
    if (commit.ok) return;
  }
  throw new Error("Paid fallback request changed concurrently.");
};

const releasePaidFallbackBeforeDispatchV3 = async (
  reservation: PaidFallbackAdmissionV3,
  allowDispatchIntent: boolean,
): Promise<void> => {
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
      !requestEntry.value ||
      requestEntry.value.billing_state !== "pending" ||
      requestEntry.value.provider_request_id !== null ||
      (
        requestEntry.value.dispatch_state !== "reserved" &&
        !(allowDispatchIntent && requestEntry.value.dispatch_state === "dispatched")
      )
    ) return;
    const now = Date.now();
    let atomic = kv.atomic().check(requestEntry).set(requestKey, {
      ...requestEntry.value,
      dispatch_state: "not_dispatched",
      terminal_state: "cancelled",
      spend_microcredits: 0,
      billing_state: "not_billed",
      terminal_at_ms: requestEntry.value.terminal_at_ms ?? now,
      updated_at_ms: now,
    }).delete(pendingKey);
    atomic = atomic.check(gateEntry);
    // Deleting a pending marker must version-bump the gate even when it is
    // already due; otherwise an overlapping recompute can publish a stale
    // future timestamp for this row.
    atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    if (windowEntry.value) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...windowEntry.value,
        reserved_microcredits: Math.max(
          0,
          windowEntry.value.reserved_microcredits - requestEntry.value.reserved_microcredits,
        ),
        pending_count: Math.max(0, windowEntry.value.pending_count - 1),
        updated_at_ms: now,
      });
    }
    if ((await atomic.commit()).ok) return;
  }
  throw new Error("Paid fallback release changed concurrently.");
};

export const releaseUndispatchedPaidFallbackV3 = async (reservation: PaidFallbackAdmissionV3): Promise<void> =>
  await releasePaidFallbackBeforeDispatchV3(reservation, false);

/**
 * Releases a reservation after a durable dispatch intent was written but
 * before provider fetch was invoked. This must never be called once provider
 * fetch can have started.
 */
export const releasePaidFallbackBeforeProviderFetchV3 = async (
  reservation: PaidFallbackAdmissionV3,
): Promise<void> => await releasePaidFallbackBeforeDispatchV3(reservation, true);

const acquireReconciliationLease = async (
  kv: Deno.Kv,
  keyId: string,
  now: number,
): Promise<PaidFallbackReconciliationLeaseV3 | null> => {
  const key = paidFallbackReconciliationLeaseV3Key(keyId);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackReconciliationLeaseV3>(key, { consistency: "strong" });
    if (entry.value && entry.value.expires_at_ms > now) return null;
    const lease = {
      token: crypto.randomUUID(),
      expires_at_ms: now + RECONCILIATION_LEASE_MS,
    } satisfies PaidFallbackReconciliationLeaseV3;
    const commit = await kv.atomic().check(entry).set(key, lease, {
      expireIn: RECONCILIATION_LEASE_MS,
    }).commit();
    if (commit.ok) return lease;
  }
  return null;
};

const releaseReconciliationLease = async (
  kv: Deno.Kv,
  keyId: string,
  lease: PaidFallbackReconciliationLeaseV3,
): Promise<void> => {
  const key = paidFallbackReconciliationLeaseV3Key(keyId);
  const entry = await kv.get<PaidFallbackReconciliationLeaseV3>(key, { consistency: "strong" });
  if (!entry.value || entry.value.token !== lease.token) return;
  await kv.atomic().check(entry).delete(key).commit();
};

const _expeditePaidFallbackReconciliationV3 = async (
  reservation: PaidFallbackAdmissionV3,
  now: number,
): Promise<void> => {
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
      atomic = atomic.set(
        pendingKey,
        {
          ...pending.value,
          next_reconciliation_at_ms: now,
        } satisfies PaidFallbackPendingV3,
      );
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

const deferPaidFallbackReconciliationV3 = async (
  kv: Deno.Kv,
  keyId: string,
  requestId: string,
  now: number,
): Promise<number | null> => {
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
    const atomic = kv.atomic()
      .check(requestEntry)
      .check(pendingEntry)
      .set(requestKey, {
        ...request,
        reconciliation_attempts: attempts,
        last_reconciliation_at_ms: now,
        billing_state: unresolved ? "unresolved" : request.billing_state,
        updated_at_ms: now,
      })
      .set(
        pendingKey,
        {
          ...pendingEntry.value,
          next_reconciliation_at_ms: now + delay,
        } satisfies PaidFallbackPendingV3,
      );
    if ((await atomic.commit()).ok) return delay;
  }
  throw new Error("Paid fallback reconciliation deferral changed concurrently.");
};

const settlePaidFallbackRequestV3 = async (
  kv: Deno.Kv,
  keyId: string,
  requestId: string,
  providerLog: MeteredTokenLogEntry,
  now: number,
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
    if (!request || request.billing_state === "settled" || request.billing_state === "not_billed") {
      if (pendingEntry.value) {
        let cleanup = kv.atomic().check(pendingEntry).delete(pendingKey);
        cleanup = cleanup.check(gateEntry);
        cleanup = cleanup.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
        const cleanupResult = await cleanup.commit();
        if (!cleanupResult.ok) continue;
      }
      return { settled: false, retry_delay_ms: null };
    }
    if (!pendingEntry.value || request.provider_request_id !== providerLog.request_id) {
      return { settled: false, retry_delay_ms: null };
    }
    const spend = Math.round(providerLog.quota * MICROCREDITS_PER_CREDIT / request.quota_per_credit);
    if (!Number.isSafeInteger(spend) || spend < 0) {
      return {
        settled: false,
        retry_delay_ms: await deferPaidFallbackReconciliationV3(kv, keyId, requestId, now),
      };
    }
    const windowKey = paidFallbackWindowV3Key(keyId, request.window_reset_at_ms);
    const windowEntry = await kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" });
    const dispatchedAtMs = request.dispatched_at_ms ??
      Math.max(request.created_at_ms, providerLog.created_at * 1_000);
    let atomic = kv.atomic().check(requestEntry).check(pendingEntry).set(requestKey, {
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
    }).delete(pendingKey);
    atomic = atomic.check(gateEntry);
    // Settlement removes billable work. Keep the gate due and version it so a
    // concurrent recompute cannot resurrect this marker's stale future time.
    atomic = atomic.set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now));
    if (windowEntry.value) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...windowEntry.value,
        settled_microcredits: windowEntry.value.settled_microcredits + spend,
        reserved_microcredits: Math.max(
          0,
          windowEntry.value.reserved_microcredits - request.reserved_microcredits,
        ),
        pending_count: Math.max(0, windowEntry.value.pending_count - 1),
        updated_at_ms: now,
      });
    }
    if ((await atomic.commit()).ok) return { settled: true, retry_delay_ms: null };
  }
  throw new Error("Paid fallback settlement changed concurrently.");
};

export type PaidFallbackUsageSettlementV3 = Readonly<{
  provider_request_id: string;
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
export const settlePaidFallbackUsageV3 = async (
  reservation: PaidFallbackAdmissionV3,
  usage: PaidFallbackUsageSettlementV3,
): Promise<boolean> => {
  const kv = await getKv();
  if (!kv) return false;
  const result = await settlePaidFallbackRequestV3(
    kv,
    reservation.key_id,
    reservation.request_id,
    {
      request_id: usage.provider_request_id,
      quota: usage.provider_quota,
      prompt_tokens: usage.input_tokens,
      ...(usage.cached_input_tokens === null || usage.cached_input_tokens === undefined
        ? {}
        : { cached_prompt_tokens: usage.cached_input_tokens }),
      completion_tokens: usage.output_tokens,
      model: usage.model,
      created_at: Math.max(0, Math.trunc(usage.created_at_ms / 1_000)),
    },
    Date.now(),
  );
  return result.settled;
};

export const reconcilePaidFallbackV3 = async (
  keyId: string,
  now = Date.now(),
  kvOverride?: Deno.Kv | null,
  options?: Readonly<{ skipGateRecompute?: boolean }>,
): Promise<number> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  const lease = await acquireReconciliationLease(kv, keyId, now);
  if (!lease) return 0;
  try {
    const due: Deno.KvEntry<PaidFallbackPendingV3>[] = [];
    for await (
      const pending of kv.list<PaidFallbackPendingV3>({ prefix: paidFallbackPendingV3Prefix(keyId) })
    ) {
      if (pending.value.next_reconciliation_at_ms <= now) due.push(pending);
    }
    if (!due.length) return 0;

    const candidates: Deno.KvEntry<PaidFallbackRequestV3>[] = [];
    for (const pending of due) {
      const requestId = String(pending.key.at(-1));
      const requestKey = paidFallbackRequestV3Key(keyId, requestId);
      const requestEntry = await kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" });
      const request = requestEntry.value;
      if (!request || request.billing_state === "settled" || request.billing_state === "not_billed") {
        const gateKey = paidFallbackReconciliationGateV3Key();
        const gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
        await kv.atomic()
          .check(pending)
          .check(gateEntry)
          .delete(pending.key)
          .set(gateKey, paidFallbackReconciliationGateDueNow(gateEntry, now))
          .commit();
        continue;
      }
      candidates.push(requestEntry as Deno.KvEntry<PaidFallbackRequestV3>);
    }
    if (!candidates.length) return 0;
    const providerRequestIds = candidates
      .map((requestEntry) => requestEntry.value.provider_request_id)
      .filter((requestId): requestId is string => requestId !== null);
    let logs: readonly MeteredTokenLogEntry[] = [];
    try {
      logs = providerRequestIds.length
        ? await fetchMeteredTokenLogs({
          requestIds: providerRequestIds,
          startAtMs: Math.min(...candidates.map((requestEntry) => requestEntry.value.created_at_ms)) - 60_000,
          endAtMs: now + 60_000,
        })
        : [];
    } catch {
      await Promise.all(
        candidates.map(async (requestEntry) => {
          await deferPaidFallbackReconciliationV3(
            kv,
            keyId,
            requestEntry.value.request_id,
            now,
          );
        }),
      );
      // The durable marker carries the retry timestamp. New Deno Deploy
      // reconciles it from cron because KV queue delivery is unavailable.
      return 0;
    }
    const byId = new Map(logs.map((log) => [log.request_id, log]));
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
    // The durable marker carries the retry timestamp. New Deno Deploy
    // reconciles it from cron because KV queue delivery is unavailable.
    return settled;
  } finally {
    await releaseReconciliationLease(kv, keyId, lease);
    if (!options?.skipGateRecompute) await recomputePaidFallbackReconciliationGateV3(kv);
  }
};

export const reconcileDuePaidFallbacksV3 = async (
  now = Date.now(),
  kvOverride?: Deno.Kv | null,
): Promise<number> => {
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  const gateKey = paidFallbackReconciliationGateV3Key();
  let gateEntry: Deno.KvEntryMaybe<PaidFallbackReconciliationGateV3> | null = null;
  let bootstrapScan: PendingMarkerScan | null = null;
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    gateEntry = await kv.get<PaidFallbackReconciliationGateV3>(gateKey, { consistency: "strong" });
    if (isPaidFallbackReconciliationGate(gateEntry.value)) break;
    bootstrapScan = await scanPaidFallbackPendingMarkers(kv);
    const commit = await kv.atomic()
      .check(gateEntry)
      .set(
        gateKey,
        { next_due_at_ms: bootstrapScan.earliest_due_at_ms } satisfies PaidFallbackReconciliationGateV3,
      )
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

  const pendingScan = bootstrapScan ?? await scanPaidFallbackPendingMarkers(kv);
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
  provider?: PaidFallbackProvider,
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
  provider?: PaidFallbackProvider,
): Promise<number> => {
  await markPaidFallbackTerminalV3(reservation, terminalState, provider);
  // Legacy direct callers are retained only for deterministic local migration
  // tests; production inference calls markPaidFallbackTerminalV3 instead.
  return await reconcilePaidFallbackV3(reservation.key_id, Date.now());
};

export const handlePaidFallbackReconciliationJobV3 = async (
  message: unknown,
  kvOverride?: Deno.Kv | null,
): Promise<number> => {
  if (!message || typeof message !== "object") return 0;
  const keyId = "key_id" in message && typeof message.key_id === "string" ? message.key_id : null;
  if (!keyId) return 0;
  const kv = await resolveKv(kvOverride);
  if (!kv) return 0;
  return await reconcilePaidFallbackV3(keyId, Date.now(), kv);
};

export const enqueueDuePaidFallbackReconciliationJobsV3 = async (
  now = Date.now(),
  kvOverride?: Deno.Kv | null,
): Promise<number> => {
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
