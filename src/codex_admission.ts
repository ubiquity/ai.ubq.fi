import { type CodexQuotaClass } from "./codex_account_routing.ts";
import { STREAM_INACTIVITY_DEADLINE_MS } from "./inference_deadline.ts";
import { getKv } from "./kv.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";

export const CODEX_ADMISSION_PREFIX = ["uos_ai", "codex_admission", "v1"] as const;
export const CODEX_ADMISSION_ACCOUNT_LIMIT = 4;
export const CODEX_ADMISSION_RENEW_AFTER_MS = 60_000;
export const CODEX_ADMISSION_LEASE_SAFETY_MARGIN_MS = 30_000;
export const CODEX_ADMISSION_LEASE_MS = CODEX_ADMISSION_RENEW_AFTER_MS +
  STREAM_INACTIVITY_DEADLINE_MS + CODEX_ADMISSION_LEASE_SAFETY_MARGIN_MS;
export const CODEX_ADMISSION_RETRY_AFTER_SECONDS = 1;
export const CODEX_ADMISSION_KV_TIMEOUT_MS = 2_000;
const CODEX_ADMISSION_LOCAL_ACCOUNT_LIMIT = 1;
const CODEX_ADMISSION_MAX_CAS_ATTEMPTS = 4;

type CodexAdmissionRecord = Readonly<{
  v: 1;
  token: string;
  account_id_hash: string;
  quota_class: CodexQuotaClass;
  caller_lane_hash: string;
  slot: number;
  acquired_at_ms: number;
  expires_at_ms: number;
}>;

export type CodexAdmissionLease = Readonly<{
  backend: "kv" | "local";
  token: string;
  accountIdHash: string;
  quotaClass: CodexQuotaClass;
  callerLaneHash: string;
  slot: number;
  expiresAtMs: number;
  slotKey: Deno.KvKey;
  callerKey: Deno.KvKey;
  kv: Deno.Kv | null;
}>;

export type CodexAdmissionDecision =
  | Readonly<{ kind: "acquired"; lease: CodexAdmissionLease }>
  | Readonly<{ kind: "account_busy" }>
  | Readonly<{ kind: "caller_busy" }>
  | Readonly<{ kind: "unavailable" }>;

type CodexAdmissionDependencies = Readonly<{
  kv?: Deno.Kv | null;
  now?: () => number;
  newToken?: () => string;
  accountLimit?: number;
  leaseMs?: number;
  kvTimeoutMs?: number;
  signal?: AbortSignal;
  /** Explicit test/local seam. Production callers fail closed when KV is unavailable. */
  allowLocalFallback?: boolean;
}>;

const localSlots = new Map<string, CodexAdmissionRecord>();
const localCallers = new Map<string, CodexAdmissionRecord>();
let admissionTestNamespace = 0;

const admissionPrefix = (): Deno.KvKey =>
  admissionTestNamespace === 0
    ? [...CODEX_ADMISSION_PREFIX]
    : [...CODEX_ADMISSION_PREFIX, "test", admissionTestNamespace];

type AdmissionDeadline = Readonly<{ signal: AbortSignal; clear: () => void }>;

const createAdmissionDeadline = (
  signal: AbortSignal | undefined,
  timeoutMs: number,
): AdmissionDeadline => {
  const timeout = new AbortController();
  const timer = setTimeout(
    () => timeout.abort(new DOMException("Codex admission KV operation timed out.", "TimeoutError")),
    Math.max(1, Math.trunc(timeoutMs)),
  );
  return {
    signal: signal ? AbortSignal.any([signal, timeout.signal]) : timeout.signal,
    clear: () => clearTimeout(timer),
  };
};

const awaitAdmissionOperation = async <T>(operation: Promise<T>, signal: AbortSignal): Promise<T> => {
  if (signal.aborted) throw signal.reason ?? new DOMException("Codex admission was aborted.", "AbortError");
  let onAbort = (): void => {};
  try {
    return await Promise.race([
      operation,
      new Promise<never>((_, reject) => {
        onAbort = () => reject(signal.reason ?? new DOMException("Codex admission was aborted.", "AbortError"));
        signal.addEventListener("abort", onAbort, { once: true });
      }),
    ]);
  } finally {
    signal.removeEventListener("abort", onAbort);
  }
};

const encodedKey = (key: Deno.KvKey): string => JSON.stringify(key);

const isSafeNonNegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const parseAdmissionRecord = (value: unknown): CodexAdmissionRecord | null => {
  if (!isRecord(value)) return null;
  if (
    value.v !== 1 || typeof value.token !== "string" || !value.token ||
    typeof value.account_id_hash !== "string" || !value.account_id_hash ||
    (value.quota_class !== "spark" && value.quota_class !== "gpt_oss_120b" &&
      value.quota_class !== "standard" && value.quota_class !== "unknown") ||
    typeof value.caller_lane_hash !== "string" || !value.caller_lane_hash ||
    !isSafeNonNegativeInteger(value.slot) ||
    !isSafeNonNegativeInteger(value.acquired_at_ms) ||
    !isSafeNonNegativeInteger(value.expires_at_ms) || value.expires_at_ms <= value.acquired_at_ms
  ) return null;
  return value as CodexAdmissionRecord;
};

export const codexAdmissionSlotKey = (
  accountIdHash: string,
  slot: number,
): Deno.KvKey => [...admissionPrefix(), "account", accountIdHash, slot];

export const codexAdmissionCallerKey = (
  callerLaneHash: string,
): Deno.KvKey => [...admissionPrefix(), "caller", callerLaneHash];

const normalizedMetadataValue = (metadata: Record<string, unknown> | null, key: string): string | null => {
  const value = getString(metadata?.[key])?.trim() ?? "";
  return value || null;
};

/**
 * Builds an opaque fairness lane. Codex emits one `thread_id` per agent while
 * descendants share `session_id`, so thread identity must take precedence.
 * The authenticated principal always scopes a caller-controlled hint.
 */
export const deriveCodexAdmissionCallerLaneHash = async (
  principal: string | null | undefined,
  clientMetadata: unknown,
  promptCacheKey: unknown,
): Promise<string> => {
  const metadata = isRecord(clientMetadata) ? clientMetadata : null;
  const threadId = normalizedMetadataValue(metadata, "thread_id");
  const sessionId = normalizedMetadataValue(metadata, "session_id");
  const promptCache = getString(promptCacheKey)?.trim() || null;
  const scope = principal?.trim() || "shared-authenticated-caller";
  const hintKind = threadId ? "thread" : sessionId ? "session" : promptCache ? "prompt_cache" : "principal";
  const hint = threadId ?? sessionId ?? promptCache ?? scope;
  return await sha256Hex(`uos-codex-admission-caller-v1\u0000${scope}\u0000${hintKind}\u0000${hint}`);
};

const slotStart = (callerLaneHash: string, accountLimit: number): number => {
  const prefix = Number.parseInt(callerLaneHash.slice(0, 8), 16);
  return Number.isFinite(prefix) ? prefix % accountLimit : 0;
};

const recordFor = (
  token: string,
  input: Readonly<{
    accountIdHash: string;
    quotaClass: CodexQuotaClass;
    callerLaneHash: string;
  }>,
  slot: number,
  nowMs: number,
  leaseMs: number,
): CodexAdmissionRecord => ({
  v: 1,
  token,
  account_id_hash: input.accountIdHash,
  quota_class: input.quotaClass,
  caller_lane_hash: input.callerLaneHash,
  slot,
  acquired_at_ms: nowMs,
  expires_at_ms: nowMs + leaseMs,
});

const leaseFor = (
  backend: "kv" | "local",
  record: CodexAdmissionRecord,
  kv: Deno.Kv | null,
): CodexAdmissionLease => ({
  backend,
  token: record.token,
  accountIdHash: record.account_id_hash,
  quotaClass: record.quota_class,
  callerLaneHash: record.caller_lane_hash,
  slot: record.slot,
  expiresAtMs: record.expires_at_ms,
  slotKey: codexAdmissionSlotKey(record.account_id_hash, record.slot),
  callerKey: codexAdmissionCallerKey(record.caller_lane_hash),
  kv,
});

const compensateLateAdmissionCommit = (
  commit: Promise<Readonly<{ ok: boolean }>>,
  lease: CodexAdmissionLease,
): void => {
  void commit.then((result) => {
    if (!result.ok) return;
    // Deno KV operations are not cancellable. If the caller deadline wins but
    // the atomic write is accepted later, release the token-fenced lease as
    // soon as its acknowledgement arrives instead of leaving ghost capacity.
    void releaseCodexAdmission(lease).catch(() => {});
  }).catch(() => {});
};

const acquireLocalAdmission = (
  input: Readonly<{
    accountIdHash: string;
    quotaClass: CodexQuotaClass;
    callerLaneHash: string;
  }>,
  nowMs: number,
  token: string,
  leaseMs: number,
): CodexAdmissionDecision => {
  const callerKey = codexAdmissionCallerKey(input.callerLaneHash);
  const encodedCaller = encodedKey(callerKey);
  const caller = localCallers.get(encodedCaller);
  if (caller && caller.expires_at_ms > nowMs) return { kind: "caller_busy" };
  if (caller) localCallers.delete(encodedCaller);

  const accountLimit = CODEX_ADMISSION_LOCAL_ACCOUNT_LIMIT;
  const start = slotStart(input.callerLaneHash, accountLimit);
  for (let offset = 0; offset < accountLimit; offset += 1) {
    const slot = (start + offset) % accountLimit;
    const slotKey = codexAdmissionSlotKey(input.accountIdHash, slot);
    const encodedSlot = encodedKey(slotKey);
    const current = localSlots.get(encodedSlot);
    if (current && current.expires_at_ms > nowMs) continue;
    if (current) localSlots.delete(encodedSlot);
    const record = recordFor(token, input, slot, nowMs, leaseMs);
    localSlots.set(encodedSlot, record);
    localCallers.set(encodedCaller, record);
    return { kind: "acquired", lease: leaseFor("local", record, null) };
  }
  return { kind: "account_busy" };
};

export const acquireCodexAdmission = async (
  input: Readonly<{
    accountIdHash: string;
    quotaClass: CodexQuotaClass;
    callerLaneHash: string;
  }>,
  dependencies: CodexAdmissionDependencies = {},
): Promise<CodexAdmissionDecision> => {
  const now = dependencies.now ?? Date.now;
  const nowMs = now();
  const token = dependencies.newToken?.() ?? crypto.randomUUID();
  const leaseMs = Math.max(1, Math.trunc(dependencies.leaseMs ?? CODEX_ADMISSION_LEASE_MS));
  const accountLimit = Math.max(1, Math.trunc(dependencies.accountLimit ?? CODEX_ADMISSION_ACCOUNT_LIMIT));
  const deadline = createAdmissionDeadline(
    dependencies.signal,
    dependencies.kvTimeoutMs ?? CODEX_ADMISSION_KV_TIMEOUT_MS,
  );
  let kv: Deno.Kv | null;
  try {
    kv = dependencies.kv === undefined ? await awaitAdmissionOperation(getKv(), deadline.signal) : dependencies.kv;
  } catch (error) {
    deadline.clear();
    if (dependencies.signal?.aborted) throw dependencies.signal.reason ?? error;
    kv = null;
  }
  if (!kv) {
    deadline.clear();
    return dependencies.allowLocalFallback
      ? acquireLocalAdmission(input, nowMs, token, leaseMs)
      : { kind: "unavailable" };
  }

  const callerKey = codexAdmissionCallerKey(input.callerLaneHash);
  const firstSlot = slotStart(input.callerLaneHash, accountLimit);
  try {
    for (let attempt = 0; attempt < CODEX_ADMISSION_MAX_CAS_ATTEMPTS; attempt += 1) {
      const callerEntry = await awaitAdmissionOperation(
        kv.get<CodexAdmissionRecord>(callerKey, { consistency: "strong" }),
        deadline.signal,
      );
      if (callerEntry.value !== null) {
        return parseAdmissionRecord(callerEntry.value) ? { kind: "caller_busy" } : { kind: "unavailable" };
      }

      let sawConflict = false;
      for (let offset = 0; offset < accountLimit; offset += 1) {
        const slot = (firstSlot + offset) % accountLimit;
        const slotKey = codexAdmissionSlotKey(input.accountIdHash, slot);
        const slotEntry = await awaitAdmissionOperation(
          kv.get<CodexAdmissionRecord>(slotKey, { consistency: "strong" }),
          deadline.signal,
        );
        if (slotEntry.value !== null) {
          if (!parseAdmissionRecord(slotEntry.value)) return { kind: "unavailable" };
          // Deno KV TTL is the cross-isolate expiry authority. A caller clock
          // must never overwrite a valid record that the server still holds.
          continue;
        }
        const checkedAtMs = now();
        const record = recordFor(token, input, slot, checkedAtMs, leaseMs);
        const commitPromise = kv.atomic()
          .check(callerEntry)
          .check(slotEntry)
          .set(callerKey, record, { expireIn: leaseMs })
          .set(slotKey, record, { expireIn: leaseMs })
          .commit();
        let commit: Deno.KvCommitResult | Deno.KvCommitError;
        try {
          commit = await awaitAdmissionOperation(commitPromise, deadline.signal);
        } catch (error) {
          compensateLateAdmissionCommit(commitPromise, leaseFor("kv", record, kv));
          throw error;
        }
        if (commit.ok) return { kind: "acquired", lease: leaseFor("kv", record, kv) };
        sawConflict = true;
        break;
      }
      if (!sawConflict) return { kind: "account_busy" };
    }
    return { kind: "account_busy" };
  } catch (error) {
    if (dependencies.signal?.aborted) throw dependencies.signal.reason ?? error;
    // A failed distributed transaction may have an unknown external cause.
    // Do not create a second isolate-local lease after attempting KV writes.
    return { kind: "unavailable" };
  } finally {
    deadline.clear();
  }
};

export const renewCodexAdmission = async (
  lease: CodexAdmissionLease,
  dependencies: Readonly<{
    now?: () => number;
    leaseMs?: number;
    kvTimeoutMs?: number;
    signal?: AbortSignal;
  }> = {},
): Promise<CodexAdmissionLease | null> => {
  const now = dependencies.now ?? Date.now;
  const nowMs = now();
  if (lease.expiresAtMs <= nowMs) return null;
  if (lease.expiresAtMs - nowMs > CODEX_ADMISSION_LEASE_MS - CODEX_ADMISSION_RENEW_AFTER_MS) return lease;
  const leaseMs = Math.max(1, Math.trunc(dependencies.leaseMs ?? CODEX_ADMISSION_LEASE_MS));

  if (lease.backend === "local" || !lease.kv) {
    const slot = localSlots.get(encodedKey(lease.slotKey));
    const caller = localCallers.get(encodedKey(lease.callerKey));
    if (slot?.token !== lease.token || caller?.token !== lease.token) return null;
    const renewed = { ...slot, acquired_at_ms: nowMs, expires_at_ms: nowMs + leaseMs };
    localSlots.set(encodedKey(lease.slotKey), renewed);
    localCallers.set(encodedKey(lease.callerKey), renewed);
    return leaseFor("local", renewed, null);
  }

  const deadline = createAdmissionDeadline(
    dependencies.signal,
    dependencies.kvTimeoutMs ?? CODEX_ADMISSION_KV_TIMEOUT_MS,
  );
  try {
    const [callerEntry, slotEntry] = await awaitAdmissionOperation(
      lease.kv.getMany<[CodexAdmissionRecord, CodexAdmissionRecord]>(
        [lease.callerKey, lease.slotKey],
        { consistency: "strong" },
      ),
      deadline.signal,
    );
    const caller = parseAdmissionRecord(callerEntry.value);
    const slot = parseAdmissionRecord(slotEntry.value);
    if (caller?.token !== lease.token || slot?.token !== lease.token) return null;
    const renewed = { ...slot, acquired_at_ms: nowMs, expires_at_ms: nowMs + leaseMs };
    const commitPromise = lease.kv.atomic()
      .check(callerEntry)
      .check(slotEntry)
      .set(lease.callerKey, renewed, { expireIn: leaseMs })
      .set(lease.slotKey, renewed, { expireIn: leaseMs })
      .commit();
    let commit: Deno.KvCommitResult | Deno.KvCommitError;
    try {
      commit = await awaitAdmissionOperation(commitPromise, deadline.signal);
    } catch {
      compensateLateAdmissionCommit(commitPromise, leaseFor("kv", renewed, lease.kv));
      return null;
    }
    return commit.ok ? leaseFor("kv", renewed, lease.kv) : null;
  } catch {
    return null;
  } finally {
    deadline.clear();
  }
};

const releaseLocalAdmission = (lease: CodexAdmissionLease): boolean => {
  let released = false;
  const encodedSlot = encodedKey(lease.slotKey);
  if (localSlots.get(encodedSlot)?.token === lease.token) {
    localSlots.delete(encodedSlot);
    released = true;
  }
  const encodedCaller = encodedKey(lease.callerKey);
  if (localCallers.get(encodedCaller)?.token === lease.token) {
    localCallers.delete(encodedCaller);
    released = true;
  }
  return released;
};

export const releaseCodexAdmission = async (
  lease: CodexAdmissionLease,
  dependencies: Readonly<{ kvTimeoutMs?: number; signal?: AbortSignal }> = {},
): Promise<boolean> => {
  if (lease.backend === "local" || !lease.kv) return releaseLocalAdmission(lease);
  const kv = lease.kv;
  const deadline = createAdmissionDeadline(
    dependencies.signal,
    dependencies.kvTimeoutMs ?? CODEX_ADMISSION_KV_TIMEOUT_MS,
  );
  try {
    for (let attempt = 0; attempt < CODEX_ADMISSION_MAX_CAS_ATTEMPTS; attempt += 1) {
      const [callerEntry, slotEntry] = await awaitAdmissionOperation(
        kv.getMany<[CodexAdmissionRecord, CodexAdmissionRecord]>(
          [lease.callerKey, lease.slotKey],
          { consistency: "strong" },
        ),
        deadline.signal,
      );
      const callerMatches = parseAdmissionRecord(callerEntry.value)?.token === lease.token;
      const slotMatches = parseAdmissionRecord(slotEntry.value)?.token === lease.token;
      if (!callerMatches && !slotMatches) return false;
      let operation = kv.atomic();
      if (callerMatches) operation = operation.check(callerEntry).delete(lease.callerKey);
      if (slotMatches) operation = operation.check(slotEntry).delete(lease.slotKey);
      const commit = await awaitAdmissionOperation(operation.commit(), deadline.signal);
      if (commit.ok) return true;
    }
    return false;
  } catch {
    // Both records expire automatically. A release failure must not replace
    // an inference response or let a stale token delete a newer lease.
    return false;
  } finally {
    deadline.clear();
  }
};

export const resetCodexAdmissionForTest = (): void => {
  localSlots.clear();
  localCallers.clear();
  admissionTestNamespace += 1;
};
