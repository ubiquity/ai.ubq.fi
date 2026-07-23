import { MICROCREDITS_PER_CREDIT, PAID_FALLBACK_NO_LIMIT } from "./api_keys.ts";
import { kvPromise } from "./kv.ts";
import type { PaidFallbackRequestV3, PaidFallbackWindowV3 } from "./types.ts";
import { fetchYunwuTokenLogs } from "./yunwu.ts";

const PREFIX = ["uos_ai", "paid_fallback", "v3"] as const;
const MAX_CAS_ATTEMPTS = 128;
const RETRY_DELAYS_MS = [5_000, 30_000, 120_000, 600_000, 3_600_000, 21_600_000] as const;
const UNRESOLVED_AFTER_MS = 24 * 60 * 60_000;

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
const paidFallbackPendingV3Prefix = (keyId: string): Deno.KvKey => [...PREFIX, "pending", keyId];

export type PaidFallbackAdmissionV3 = Readonly<{
  key_id: string;
  request_id: string;
  created_at_ms: number;
  reserved_microcredits: number;
  quota_per_credit: number;
  window_reset_at_ms: number;
  quota_used_percent: number | null;
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
}>;

const enqueueReconciliation = (atomic: Deno.AtomicOperation, keyId: string): Deno.AtomicOperation => {
  const operation = atomic as Deno.AtomicOperation & {
    enqueue?: (value: unknown, options?: { delay?: number }) => Deno.AtomicOperation;
  };
  return operation.enqueue?.({ type: "paid_fallback_reconcile_v3", key_id: keyId }) ?? atomic;
};

export const admitPaidFallbackV3 = async (
  input: AdmissionInput,
): Promise<
  | Readonly<{ kind: "reserved"; reservation: PaidFallbackAdmissionV3 }>
  | Readonly<{ kind: "blocked"; reason: "limit_exceeded" | "invalid_policy" | "concurrent_update" }>
> => {
  const kv = await kvPromise;
  if (!kv) return { kind: "blocked", reason: "invalid_policy" };
  const unlimited = input.limitMicrocredits === PAID_FALLBACK_NO_LIMIT;
  if (
    !unlimited &&
    (!Number.isSafeInteger(input.maximumExposureMicrocredits) || input.maximumExposureMicrocredits! <= 0)
  ) {
    return { kind: "blocked", reason: "invalid_policy" };
  }
  const requestKey = paidFallbackRequestV3Key(input.keyId, input.requestId);
  const pendingKey = paidFallbackPendingV3Key(input.keyId, input.requestId);
  const windowKey = paidFallbackWindowV3Key(input.keyId, input.windowResetAtMs);

  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, windowEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      unlimited
        ? Promise.resolve(
          { key: windowKey, value: null, versionstamp: null } as Deno.KvEntryMaybe<PaidFallbackWindowV3>,
        )
        : kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" }),
    ]);
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
    if (
      !unlimited &&
      (current.policy_version !== input.policyVersion || current.limit_microcredits !== input.limitMicrocredits)
    ) {
      return { kind: "blocked", reason: "invalid_policy" };
    }
    const remaining = unlimited
      ? 0
      : current.limit_microcredits - current.settled_microcredits - current.reserved_microcredits;
    if (!unlimited && remaining <= 0) return { kind: "blocked", reason: "limit_exceeded" };
    const reservation = unlimited ? 0 : Math.min(remaining, input.maximumExposureMicrocredits!);
    const now = Date.now();
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
      dispatch_state: "reserved",
      terminal_state: "pending",
      spend_microcredits: null,
      billing_state: "pending",
      reconciliation_attempts: 0,
      last_reconciliation_at_ms: null,
      created_at_ms: input.createdAtMs,
      updated_at_ms: now,
    };
    let atomic = kv.atomic().check(requestEntry).set(requestKey, request).set(pendingKey, { created_at_ms: now });
    if (!unlimited) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...current,
        reserved_microcredits: current.reserved_microcredits + reservation,
        pending_count: current.pending_count + 1,
        updated_at_ms: now,
      });
    }
    const commit = await enqueueReconciliation(atomic, input.keyId).commit();
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
            : 100 * (current.settled_microcredits + current.reserved_microcredits) / current.limit_microcredits,
        },
      };
    }
  }
  return { kind: "blocked", reason: "concurrent_update" };
};

export const updatePaidFallbackRequestV3 = async (
  reservation: PaidFallbackAdmissionV3,
  patch: Partial<PaidFallbackRequestV3>,
): Promise<void> => {
  const kv = await kvPromise;
  if (!kv) return;
  const key = paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await kv.get<PaidFallbackRequestV3>(key, { consistency: "strong" });
    if (!entry.value || entry.value.billing_state !== "pending") return;
    const commit = await kv.atomic().check(entry).set(key, {
      ...entry.value,
      ...patch,
      updated_at_ms: Date.now(),
    }).commit();
    if (commit.ok) return;
  }
  throw new Error("Paid fallback request changed concurrently.");
};

export const releaseUndispatchedPaidFallbackV3 = async (reservation: PaidFallbackAdmissionV3): Promise<void> => {
  const kv = await kvPromise;
  if (!kv) return;
  const requestKey = paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
  const pendingKey = paidFallbackPendingV3Key(reservation.key_id, reservation.request_id);
  const windowKey = paidFallbackWindowV3Key(reservation.key_id, reservation.window_reset_at_ms);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const [requestEntry, windowEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" }),
    ]);
    if (!requestEntry.value || requestEntry.value.billing_state !== "pending") return;
    let atomic = kv.atomic().check(requestEntry).set(requestKey, {
      ...requestEntry.value,
      dispatch_state: "not_dispatched",
      terminal_state: "cancelled",
      spend_microcredits: 0,
      billing_state: "not_billed",
      updated_at_ms: Date.now(),
    }).delete(pendingKey);
    if (windowEntry.value) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...windowEntry.value,
        reserved_microcredits: Math.max(0, windowEntry.value.reserved_microcredits - reservation.reserved_microcredits),
        pending_count: Math.max(0, windowEntry.value.pending_count - 1),
        updated_at_ms: Date.now(),
      });
    }
    if ((await atomic.commit()).ok) return;
  }
  throw new Error("Paid fallback release changed concurrently.");
};

export const reconcilePaidFallbackV3 = async (keyId: string): Promise<number> => {
  const kv = await kvPromise;
  if (!kv) return 0;
  const logs = await fetchYunwuTokenLogs();
  const byId = new Map(logs.map((log) => [log.request_id, log]));
  let settled = 0;
  for await (const pending of kv.list({ prefix: paidFallbackPendingV3Prefix(keyId) })) {
    const requestId = String(pending.key.at(-1));
    const requestKey = paidFallbackRequestV3Key(keyId, requestId);
    const requestEntry = await kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" });
    const request = requestEntry.value;
    if (!request || request.billing_state !== "pending") {
      await kv.delete(pending.key);
      continue;
    }
    const providerLog = request.provider_request_id ? byId.get(request.provider_request_id) : null;
    const now = Date.now();
    if (!providerLog) {
      const attempts = request.reconciliation_attempts + 1;
      const unresolved = now - request.created_at_ms >= UNRESOLVED_AFTER_MS;
      await updatePaidFallbackRequestV3(
        {
          key_id: keyId,
          request_id: requestId,
          created_at_ms: request.created_at_ms,
          reserved_microcredits: request.reserved_microcredits,
          quota_per_credit: 1,
          window_reset_at_ms: request.window_reset_at_ms,
          quota_used_percent: null,
        },
        {
          reconciliation_attempts: attempts,
          last_reconciliation_at_ms: now,
          ...(unresolved ? { billing_state: "unresolved" as const } : {}),
        },
      );
      if (!unresolved) {
        const delay = RETRY_DELAYS_MS[Math.min(attempts - 1, RETRY_DELAYS_MS.length - 1)];
        await kv.enqueue({ type: "paid_fallback_reconcile_v3", key_id: keyId }, { delay });
      }
      continue;
    }
    const spend = Math.round(providerLog.quota * MICROCREDITS_PER_CREDIT / request.quota_per_credit);
    const windowKey = paidFallbackWindowV3Key(keyId, request.window_reset_at_ms);
    const windowEntry = await kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" });
    let atomic = kv.atomic().check(requestEntry).set(requestKey, {
      ...request,
      terminal_state: request.terminal_state === "pending" ? "completed" : request.terminal_state,
      spend_microcredits: spend,
      billing_state: "settled",
      reconciliation_attempts: request.reconciliation_attempts + 1,
      last_reconciliation_at_ms: now,
      updated_at_ms: now,
    }).delete(pending.key);
    if (windowEntry.value) {
      atomic = atomic.check(windowEntry).set(windowKey, {
        ...windowEntry.value,
        settled_microcredits: windowEntry.value.settled_microcredits + spend,
        reserved_microcredits: Math.max(0, windowEntry.value.reserved_microcredits - request.reserved_microcredits),
        pending_count: Math.max(0, windowEntry.value.pending_count - 1),
        updated_at_ms: now,
      });
    }
    if ((await atomic.commit()).ok) settled += 1;
  }
  return settled;
};
