import { getKv } from "./kv.ts";
import { base64UrlEncode, isRecord } from "./utils.ts";

export const SENTINEL_INCIDENT_CONTROL_KEY = ["uos_ai", "sentinel_incident", "v1", "control"] as const;
export const SENTINEL_INCIDENT_CAPTURE_REF_PREFIX = [
  "uos_ai",
  "sentinel_incident",
  "v1",
  "capture_ref",
] as const;
export const SENTINEL_INCIDENT_EVENT_PREFIX = ["uos_ai", "sentinel_incident", "v1", "event"] as const;
export const SENTINEL_INCIDENT_ACK_PREFIX = ["uos_ai", "sentinel_incident", "v1", "ack"] as const;
export const SENTINEL_INCIDENT_DEFER_PREFIX = ["uos_ai", "sentinel_incident", "v1", "defer"] as const;
export const SENTINEL_INCIDENT_DEAD_PREFIX = ["uos_ai", "sentinel_incident", "v1", "dead"] as const;
export const SENTINEL_INCIDENT_TTL_MS = 48 * 60 * 60 * 1_000;

const GITHUB_APP_ID = "4682172";
const GITHUB_INSTALLATION_ID = "155687488";
const GITHUB_REPOSITORY = "ubiquity/ai.ubq.fi";
const GITHUB_WORKFLOW = "provider-sentinel.yml";
const GITHUB_REF = "development";
const GITHUB_API_BASE = "https://api.github.com/";
const GITHUB_API_VERSION = "2026-03-10";
const DISPATCH_LEASE_MS = 60_000;
const CAPTURE_RECOVERY_MS = 5 * 60_000;
const WORKFLOW_POLL_MS = 5 * 60_000;
const WORKFLOW_ACK_GRACE_MS = 5 * 60_000;
const MAX_WORKFLOW_ATTEMPTS = 3;
export const MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS = 12;
const MAX_CAS_ATTEMPTS = 8;
const MAX_EVENTS_PER_RECONCILE = 32;
const INCIDENT_ID = /^provider-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const ACK_NONCE = /^[A-Za-z0-9_-]{43}$/;
const FULL_SHA = /^[0-9a-f]{40}$/;

type IncidentBatchState = "queued" | "dispatching" | "dispatched";

export type SentinelIncidentBatch = Readonly<{
  version: 1;
  id: string;
  state: IncidentBatchState;
  first_observed_at_ms: number;
  latest_observed_at_ms: number;
  failure_count: number;
  attempt: number;
  next_action_at_ms: number;
  lease_expires_at_ms: number | null;
  workflow_run_id: number | null;
  workflow_run_url: string | null;
  success_observed_at_ms: number | null;
  ack_nonce: string;
  /** Missing on pre-deferral v1 records and treated as zero. */
  infrastructure_deferrals?: number;
}>;

export type SentinelIncidentDeferralReason =
  | "codex_auth_preflight_failed"
  | "sentinel_infrastructure_preflight_failed";

export type SentinelIncidentControl = Readonly<{
  version: 1;
  active: SentinelIncidentBatch | null;
  pending: SentinelIncidentBatch | null;
  updated_at_ms: number;
}>;

export type SentinelIncidentCaptureReference = Readonly<{
  version: 1;
  manifest_key: Deno.KvKey;
}>;

export type SentinelIncidentFailureEvent = Readonly<{
  version: 1;
  incident_id: string;
  state: "capturing" | "ready";
  observed_at_ms: number;
  created_at_ms: number;
  ready_at_ms: number | null;
  capture_status: "pending" | "stored" | "duplicate" | "unavailable";
  capture_fingerprint: string | null;
  manifest_key: Deno.KvKey | null;
}>;

type EnvironmentReader = Readonly<{ get(name: string): string | undefined }>;
type Fetcher = (input: string | URL | Request, init?: RequestInit) => Promise<Response>;

export type SentinelIncidentDependencies = Readonly<{
  now?: () => number;
  randomUuid?: () => string;
  fetcher?: Fetcher;
  createTimeoutSignal?: (milliseconds: number) => AbortSignal;
  randomAckNonce?: () => string;
}>;

export class SentinelIncidentDeliveryError extends Error {
  constructor(readonly code: string) {
    super(code);
    this.name = "SentinelIncidentDeliveryError";
  }
}

export class SentinelIncidentAckConflict extends Error {
  constructor() {
    super("sentinel_incident_ack_conflict");
    this.name = "SentinelIncidentAckConflict";
  }
}

export class SentinelIncidentClaimConflict extends Error {
  constructor() {
    super("sentinel_incident_claim_conflict");
    this.name = "SentinelIncidentClaimConflict";
  }
}

export class SentinelIncidentDeferConflict extends Error {
  constructor() {
    super("sentinel_incident_defer_conflict");
    this.name = "SentinelIncidentDeferConflict";
  }
}

const safeEnvironment: EnvironmentReader = {
  get(name) {
    try {
      return Deno.env.get(name)?.trim() || undefined;
    } catch {
      return undefined;
    }
  },
};

const positiveInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const nonNegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const nullablePositiveInteger = (value: unknown): value is number | null => value === null || positiveInteger(value);

export const isSentinelIncidentId = (value: unknown): value is string =>
  typeof value === "string" && INCIDENT_ID.test(value);

export const isSentinelIncidentDeferralReason = (value: unknown): value is SentinelIncidentDeferralReason =>
  value === "codex_auth_preflight_failed" || value === "sentinel_infrastructure_preflight_failed";

const isWorkflowRunUrl = (value: unknown, runId: number | null): value is string | null => {
  if (value === null) return runId === null;
  return typeof value === "string" && runId !== null &&
    value === `https://github.com/${GITHUB_REPOSITORY}/actions/runs/${runId}`;
};

export const isSentinelIncidentBatch = (value: unknown): value is SentinelIncidentBatch => {
  if (!isRecord(value) || value.version !== 1 || !isSentinelIncidentId(value.id)) return false;
  if (value.state !== "queued" && value.state !== "dispatching" && value.state !== "dispatched") return false;
  if (
    !positiveInteger(value.first_observed_at_ms) || !positiveInteger(value.latest_observed_at_ms) ||
    value.latest_observed_at_ms < value.first_observed_at_ms || !positiveInteger(value.failure_count) ||
    !positiveInteger(value.attempt) || value.attempt > MAX_WORKFLOW_ATTEMPTS ||
    (value.infrastructure_deferrals !== undefined &&
      (!nonNegativeInteger(value.infrastructure_deferrals) ||
        value.infrastructure_deferrals > MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS)) ||
    !positiveInteger(value.next_action_at_ms) ||
    !nullablePositiveInteger(value.lease_expires_at_ms) || !nullablePositiveInteger(value.workflow_run_id) ||
    !nullablePositiveInteger(value.success_observed_at_ms) ||
    !isWorkflowRunUrl(value.workflow_run_url, value.workflow_run_id) ||
    typeof value.ack_nonce !== "string" || !ACK_NONCE.test(value.ack_nonce)
  ) return false;
  if (value.state === "queued") {
    return value.lease_expires_at_ms === null && value.workflow_run_id === null && value.workflow_run_url === null &&
      value.success_observed_at_ms === null;
  }
  if (value.state === "dispatching") {
    return value.lease_expires_at_ms !== null && value.workflow_run_id === null && value.workflow_run_url === null &&
      value.success_observed_at_ms === null;
  }
  return value.lease_expires_at_ms === null && value.workflow_run_id !== null && value.workflow_run_url !== null;
};

export const isSentinelIncidentControl = (value: unknown): value is SentinelIncidentControl =>
  isRecord(value) && value.version === 1 && positiveInteger(value.updated_at_ms) &&
  (value.active === null || isSentinelIncidentBatch(value.active)) &&
  (value.pending === null || isSentinelIncidentBatch(value.pending)) &&
  (value.pending === null || value.pending.state === "queued") &&
  !(value.active === null && value.pending !== null);

export const isSentinelIncidentCaptureReference = (value: unknown): value is SentinelIncidentCaptureReference =>
  isRecord(value) && value.version === 1 && Array.isArray(value.manifest_key) && value.manifest_key.length === 7;

export const isSentinelIncidentFailureEvent = (value: unknown): value is SentinelIncidentFailureEvent => {
  if (
    !isRecord(value) || value.version !== 1 || !isSentinelIncidentId(value.incident_id) ||
    !positiveInteger(value.observed_at_ms) || !positiveInteger(value.created_at_ms) ||
    value.created_at_ms < value.observed_at_ms ||
    (value.ready_at_ms !== null && !positiveInteger(value.ready_at_ms)) ||
    (value.capture_fingerprint !== null &&
      (typeof value.capture_fingerprint !== "string" || !/^[0-9a-f]{64}$/.test(value.capture_fingerprint))) ||
    (value.manifest_key !== null && (!Array.isArray(value.manifest_key) || value.manifest_key.length !== 7))
  ) return false;
  if (value.state === "capturing") {
    return value.ready_at_ms === null && value.capture_status === "pending" && value.capture_fingerprint === null &&
      value.manifest_key === null;
  }
  if (value.state !== "ready" || value.ready_at_ms === null || value.ready_at_ms < value.created_at_ms) return false;
  if (value.capture_status === "unavailable") {
    return value.capture_fingerprint === null && value.manifest_key === null;
  }
  return (value.capture_status === "stored" || value.capture_status === "duplicate") &&
    value.capture_fingerprint !== null && value.manifest_key !== null;
};

export const isSentinelProductionRuntime = (environment: EnvironmentReader = safeEnvironment): boolean =>
  environment.get("DENO_DEPLOY_ORG_SLUG") === "ubiquity-dao" &&
  environment.get("DENO_DEPLOY_APP_SLUG") === "ai-ubq-fi" &&
  environment.get("DENO_TIMELINE") === "production";

const emptyControl = (now: number): SentinelIncidentControl => ({
  version: 1,
  active: null,
  pending: null,
  updated_at_ms: now,
});

const defaultRandomUuid = (): string => crypto.randomUUID();
const defaultAckNonce = (): string => base64UrlEncode(crypto.getRandomValues(new Uint8Array(32)));

const newBatch = (
  now: number,
  randomUuid: () => string,
  randomAckNonce: () => string = defaultAckNonce,
): SentinelIncidentBatch => {
  const id = `provider-${randomUuid().toLowerCase()}`;
  if (!isSentinelIncidentId(id)) throw new Error("Sentinel incident UUID is invalid");
  const ackNonce = randomAckNonce();
  if (!ACK_NONCE.test(ackNonce)) throw new Error("Sentinel incident ACK nonce is invalid");
  return {
    version: 1,
    id,
    state: "queued",
    first_observed_at_ms: now,
    latest_observed_at_ms: now,
    failure_count: 1,
    attempt: 1,
    next_action_at_ms: now,
    lease_expires_at_ms: null,
    workflow_run_id: null,
    workflow_run_url: null,
    success_observed_at_ms: null,
    ack_nonce: ackNonce,
    infrastructure_deferrals: 0,
  };
};

const extendBatch = (batch: SentinelIncidentBatch, now: number): SentinelIncidentBatch => ({
  ...batch,
  first_observed_at_ms: Math.min(batch.first_observed_at_ms, now),
  latest_observed_at_ms: Math.max(batch.latest_observed_at_ms, now),
  failure_count: batch.failure_count + 1,
});

const controlEntry = async (kv: Deno.Kv): Promise<Deno.KvEntryMaybe<SentinelIncidentControl>> => {
  const entry = await kv.get<SentinelIncidentControl>(SENTINEL_INCIDENT_CONTROL_KEY);
  if (entry.value !== null && !isSentinelIncidentControl(entry.value)) {
    throw new Error("Sentinel incident control record is invalid");
  }
  return entry.value === null ? { ...entry, value: null } : entry;
};

const setControl = (
  kv: Deno.Kv,
  entry: Deno.KvEntryMaybe<SentinelIncidentControl>,
  value: SentinelIncidentControl,
): Promise<Deno.KvCommitResult | Deno.KvCommitError> =>
  kv.atomic()
    .check({ key: SENTINEL_INCIDENT_CONTROL_KEY, versionstamp: entry.versionstamp })
    .set(SENTINEL_INCIDENT_CONTROL_KEY, value, { expireIn: SENTINEL_INCIDENT_TTL_MS })
    .commit();

const incidentEventKey = (incidentId: string): Deno.KvKey => [...SENTINEL_INCIDENT_EVENT_PREFIX, incidentId];

export const createSentinelIncidentFailureEvent = async (
  kv: Deno.Kv,
  observedAtMs: number,
  dependencies: Pick<SentinelIncidentDependencies, "randomUuid"> = {},
): Promise<Deno.KvEntry<SentinelIncidentFailureEvent>> => {
  if (!positiveInteger(observedAtMs)) throw new Error("Sentinel incident timestamp is invalid");
  const incidentId = `provider-${(dependencies.randomUuid ?? defaultRandomUuid)().toLowerCase()}`;
  if (!isSentinelIncidentId(incidentId)) throw new Error("Sentinel incident UUID is invalid");
  const key = incidentEventKey(incidentId);
  const value: SentinelIncidentFailureEvent = {
    version: 1,
    incident_id: incidentId,
    state: "capturing",
    observed_at_ms: observedAtMs,
    created_at_ms: observedAtMs,
    ready_at_ms: null,
    capture_status: "pending",
    capture_fingerprint: null,
    manifest_key: null,
  };
  const committed = await kv.atomic()
    .check({ key, versionstamp: null })
    .set(key, value, { expireIn: SENTINEL_INCIDENT_TTL_MS })
    .commit();
  if (!committed.ok) throw new Error("Sentinel incident event identifier conflicted");
  return { key, value, versionstamp: committed.versionstamp };
};

export const recordSentinelProviderDegradation = async (
  kv: Deno.Kv,
  observedAtMs: number,
  dependencies: Pick<SentinelIncidentDependencies, "randomUuid"> = {},
): Promise<string> => {
  if (!positiveInteger(observedAtMs)) throw new Error("Sentinel incident timestamp is invalid");
  const incidentId = `provider-${(dependencies.randomUuid ?? defaultRandomUuid)().toLowerCase()}`;
  if (!isSentinelIncidentId(incidentId)) throw new Error("Sentinel incident UUID is invalid");
  const key = incidentEventKey(incidentId);
  const value: SentinelIncidentFailureEvent = {
    version: 1,
    incident_id: incidentId,
    state: "ready",
    observed_at_ms: observedAtMs,
    created_at_ms: observedAtMs,
    ready_at_ms: observedAtMs,
    capture_status: "unavailable",
    capture_fingerprint: null,
    manifest_key: null,
  };
  const committed = await kv.atomic()
    .check({ key, versionstamp: null })
    .set(key, value, { expireIn: SENTINEL_INCIDENT_TTL_MS })
    .commit();
  if (!committed.ok) throw new Error("Sentinel incident event identifier conflicted");
  return incidentId;
};

type IncidentCaptureCompletion =
  | Readonly<{ status: "stored" | "duplicate"; fingerprint: string; manifestKey: Deno.KvKey }>
  | Readonly<{ status: "unavailable" }>;

export const readySentinelIncidentFailureEvent = (
  entry: Deno.KvEntry<SentinelIncidentFailureEvent>,
  readyAtMs: number,
  capture: IncidentCaptureCompletion,
): SentinelIncidentFailureEvent => {
  if (!isSentinelIncidentFailureEvent(entry.value) || entry.value.state !== "capturing") {
    throw new Error("Sentinel incident event is not awaiting capture");
  }
  if (!positiveInteger(readyAtMs) || readyAtMs < entry.value.created_at_ms) {
    throw new Error("Sentinel incident ready timestamp is invalid");
  }
  const value: SentinelIncidentFailureEvent = capture.status === "unavailable"
    ? {
      ...entry.value,
      state: "ready",
      ready_at_ms: readyAtMs,
      capture_status: "unavailable",
    }
    : {
      ...entry.value,
      state: "ready",
      ready_at_ms: readyAtMs,
      capture_status: capture.status,
      capture_fingerprint: capture.fingerprint,
      manifest_key: [...capture.manifestKey],
    };
  if (!isSentinelIncidentFailureEvent(value)) throw new Error("Sentinel incident capture completion is invalid");
  return value;
};

export const completeSentinelIncidentFailureEvent = async (
  kv: Deno.Kv,
  entry: Deno.KvEntry<SentinelIncidentFailureEvent>,
  readyAtMs: number,
  capture: IncidentCaptureCompletion,
): Promise<boolean> => {
  const value = readySentinelIncidentFailureEvent(entry, readyAtMs, capture);
  const committed = await kv.atomic()
    .check({ key: entry.key, versionstamp: entry.versionstamp })
    .set(entry.key, value, { expireIn: SENTINEL_INCIDENT_TTL_MS })
    .commit();
  return committed.ok;
};

const batchFromFailureEvent = (
  event: SentinelIncidentFailureEvent,
  now: number,
  randomAckNonce: () => string,
): SentinelIncidentBatch => {
  const ackNonce = randomAckNonce();
  if (!ACK_NONCE.test(ackNonce)) throw new Error("Sentinel incident ACK nonce is invalid");
  return {
    version: 1,
    id: event.incident_id,
    state: "queued",
    first_observed_at_ms: event.observed_at_ms,
    latest_observed_at_ms: event.observed_at_ms,
    failure_count: 1,
    attempt: 1,
    next_action_at_ms: now,
    lease_expires_at_ms: null,
    workflow_run_id: null,
    workflow_run_url: null,
    success_observed_at_ms: null,
    ack_nonce: ackNonce,
    infrastructure_deferrals: 0,
  };
};

export const coalesceSentinelIncidentFailureEvents = async (
  kv: Deno.Kv,
  now: number,
  dependencies: Pick<SentinelIncidentDependencies, "randomAckNonce"> = {},
): Promise<number> => {
  if (!positiveInteger(now)) throw new Error("Sentinel incident clock is invalid");
  const randomAckNonce = dependencies.randomAckNonce ?? defaultAckNonce;
  let coalesced = 0;
  for await (const listed of kv.list<SentinelIncidentFailureEvent>({ prefix: SENTINEL_INCIDENT_EVENT_PREFIX })) {
    if (coalesced >= MAX_EVENTS_PER_RECONCILE) break;
    if (!isSentinelIncidentFailureEvent(listed.value)) throw new Error("Sentinel incident failure event is invalid");
    let eventEntry: Deno.KvEntry<SentinelIncidentFailureEvent> = listed;
    if (eventEntry.value.state === "capturing") {
      if (eventEntry.value.created_at_ms + CAPTURE_RECOVERY_MS > now) continue;
      const recovered = readySentinelIncidentFailureEvent(eventEntry, now, { status: "unavailable" });
      const committed = await kv.atomic()
        .check({ key: eventEntry.key, versionstamp: eventEntry.versionstamp })
        .set(eventEntry.key, recovered, { expireIn: SENTINEL_INCIDENT_TTL_MS })
        .commit();
      if (!committed.ok) continue;
      eventEntry = { key: eventEntry.key, value: recovered, versionstamp: committed.versionstamp };
    }

    let integrated = false;
    for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
      const [latestEvent, entry] = await Promise.all([
        kv.get<SentinelIncidentFailureEvent>(eventEntry.key),
        controlEntry(kv),
      ]);
      if (latestEvent.value === null) {
        integrated = true;
        break;
      }
      if (!isSentinelIncidentFailureEvent(latestEvent.value)) {
        throw new Error("Sentinel incident failure event is invalid");
      }
      if (latestEvent.value.state !== "ready") break;

      const current = entry.value ?? emptyControl(now);
      let active = current.active;
      let pending = current.pending;
      let incidentId: string;
      if (!active) {
        active = batchFromFailureEvent(latestEvent.value, now, randomAckNonce);
        incidentId = active.id;
      } else if (active.state === "queued") {
        active = extendBatch(active, latestEvent.value.observed_at_ms);
        incidentId = active.id;
      } else if (pending) {
        pending = extendBatch(pending, latestEvent.value.observed_at_ms);
        incidentId = pending.id;
      } else {
        pending = batchFromFailureEvent(latestEvent.value, now, randomAckNonce);
        incidentId = pending.id;
      }

      let operation = kv.atomic()
        .check({ key: SENTINEL_INCIDENT_CONTROL_KEY, versionstamp: entry.versionstamp })
        .check({ key: latestEvent.key, versionstamp: latestEvent.versionstamp })
        .set(
          SENTINEL_INCIDENT_CONTROL_KEY,
          {
            version: 1,
            active,
            pending,
            updated_at_ms: now,
          } satisfies SentinelIncidentControl,
          { expireIn: SENTINEL_INCIDENT_TTL_MS },
        );
      if (
        latestEvent.value.capture_status === "stored" || latestEvent.value.capture_status === "duplicate"
      ) {
        const fingerprint = latestEvent.value.capture_fingerprint!;
        const reference: SentinelIncidentCaptureReference = {
          version: 1,
          manifest_key: [...latestEvent.value.manifest_key!],
        };
        operation = operation.set(
          [...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, fingerprint],
          reference,
          { expireIn: SENTINEL_INCIDENT_TTL_MS },
        );
      }
      const committed = await operation.delete(latestEvent.key).commit();
      if (committed.ok) {
        integrated = true;
        coalesced += 1;
        break;
      }
    }
    if (!integrated) throw new Error("Sentinel incident event coalescing conflicted repeatedly");
  }
  return coalesced;
};

export const recordSentinelIncidentFailure = async (
  kv: Deno.Kv,
  observedAtMs: number,
  dependencies: Pick<SentinelIncidentDependencies, "randomUuid" | "randomAckNonce"> = {},
): Promise<Readonly<{ incidentId: string; created: boolean }>> => {
  if (!positiveInteger(observedAtMs)) throw new Error("Sentinel incident timestamp is invalid");
  const randomUuid = dependencies.randomUuid ?? defaultRandomUuid;
  const randomAckNonce = dependencies.randomAckNonce ?? defaultAckNonce;
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await controlEntry(kv);
    const current = entry.value ?? emptyControl(observedAtMs);
    let active = current.active;
    let pending = current.pending;
    let incidentId: string;
    let created = false;
    if (!active) {
      active = newBatch(observedAtMs, randomUuid, randomAckNonce);
      incidentId = active.id;
      created = true;
    } else if (active.state === "queued") {
      active = extendBatch(active, observedAtMs);
      incidentId = active.id;
    } else if (pending) {
      pending = extendBatch(pending, observedAtMs);
      incidentId = pending.id;
    } else {
      pending = newBatch(observedAtMs, randomUuid, randomAckNonce);
      incidentId = pending.id;
      created = true;
    }
    const committed = await setControl(kv, entry, {
      version: 1,
      active,
      pending,
      updated_at_ms: observedAtMs,
    });
    if (committed.ok) return { incidentId, created };
  }
  throw new Error("Sentinel incident control update conflicted repeatedly");
};

export const linkSentinelReplayToIncident = async (
  kv: Deno.Kv,
  incidentId: string,
  fingerprint: string,
  manifestKey: Deno.KvKey,
): Promise<void> => {
  if (!isSentinelIncidentId(incidentId) || !/^[0-9a-f]{64}$/.test(fingerprint)) {
    throw new Error("Sentinel incident capture reference is invalid");
  }
  const reference: SentinelIncidentCaptureReference = { version: 1, manifest_key: [...manifestKey] };
  if (!isSentinelIncidentCaptureReference(reference)) throw new Error("Sentinel replay manifest key is invalid");
  await kv.set([...SENTINEL_INCIDENT_CAPTURE_REF_PREFIX, incidentId, fingerprint], reference, {
    expireIn: SENTINEL_INCIDENT_TTL_MS,
  });
};

const derLength = (length: number): Uint8Array => {
  if (!Number.isSafeInteger(length) || length < 0) throw new Error("RSA key length is invalid");
  if (length < 0x80) return Uint8Array.of(length);
  const bytes: number[] = [];
  for (let value = length; value > 0; value >>>= 8) bytes.unshift(value & 0xff);
  return Uint8Array.of(0x80 | bytes.length, ...bytes);
};

const concat = (...parts: readonly Uint8Array[]): Uint8Array<ArrayBuffer> => {
  const output = new Uint8Array(parts.reduce((length, part) => length + part.byteLength, 0));
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
};

const derValue = (tag: number, value: Uint8Array): Uint8Array<ArrayBuffer> =>
  concat(Uint8Array.of(tag), derLength(value.byteLength), value);

const wrapPkcs1AsPkcs8 = (pkcs1: Uint8Array): Uint8Array<ArrayBuffer> => {
  const rsaAlgorithmIdentifier = Uint8Array.of(
    0x30,
    0x0d,
    0x06,
    0x09,
    0x2a,
    0x86,
    0x48,
    0x86,
    0xf7,
    0x0d,
    0x01,
    0x01,
    0x01,
    0x05,
    0x00,
  );
  return derValue(
    0x30,
    concat(Uint8Array.of(0x02, 0x01, 0x00), rsaAlgorithmIdentifier, derValue(0x04, pkcs1)),
  );
};

const decodePem = (privateKeyPem: string): Uint8Array<ArrayBuffer> => {
  const normalized = privateKeyPem.includes("\n")
    ? privateKeyPem.replaceAll("\r\n", "\n").trim()
    : privateKeyPem.replaceAll("\\n", "\n").trim();
  if (normalized.length < 100 || normalized.length > 32_768) throw new Error("GitHub App private key is invalid");
  const match = normalized.match(
    /^-----BEGIN (RSA PRIVATE KEY|PRIVATE KEY)-----\n([A-Za-z0-9+/=\n]+)\n-----END \1-----$/,
  );
  if (!match) throw new Error("GitHub App private key is invalid");
  let decoded: Uint8Array<ArrayBuffer>;
  try {
    decoded = Uint8Array.from(atob(match[2]!.replaceAll("\n", "")), (character) => character.charCodeAt(0));
  } catch {
    throw new Error("GitHub App private key is invalid");
  }
  return match[1] === "RSA PRIVATE KEY" ? wrapPkcs1AsPkcs8(decoded) : decoded;
};

const encodeJson = (value: unknown): string => base64UrlEncode(new TextEncoder().encode(JSON.stringify(value)));

export const createSentinelGitHubAppJwt = async (privateKeyPem: string, nowMs: number): Promise<string> => {
  if (!positiveInteger(nowMs)) throw new Error("GitHub App clock is invalid");
  const pkcs8 = decodePem(privateKeyPem);
  try {
    const key = await crypto.subtle.importKey(
      "pkcs8",
      pkcs8,
      { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
      false,
      ["sign"],
    );
    const nowSeconds = Math.floor(nowMs / 1_000);
    const unsigned = `${encodeJson({ alg: "RS256", typ: "JWT" })}.${
      encodeJson({ iat: nowSeconds - 60, exp: nowSeconds + 540, iss: GITHUB_APP_ID })
    }`;
    const signature = new Uint8Array(
      await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(unsigned)),
    );
    return `${unsigned}.${base64UrlEncode(signature)}`;
  } catch {
    throw new Error("GitHub App private key is invalid");
  } finally {
    pkcs8.fill(0);
  }
};

type GitHubWorkflowRun = Readonly<{
  id: number;
  status: string;
  conclusion: string | null;
  htmlUrl: string;
  headSha: string;
}>;

type GitHubAppClient = Readonly<{
  dispatch(incident: SentinelIncidentBatch): Promise<Readonly<{ runId: number; htmlUrl: string }>>;
  getRun(runId: number): Promise<GitHubWorkflowRun>;
}>;

const responseJson = async (response: Response, code: string): Promise<Record<string, unknown>> => {
  try {
    const value: unknown = await response.json();
    if (!isRecord(value)) throw new Error();
    return value;
  } catch {
    throw new SentinelIncidentDeliveryError(code);
  }
};

const request = async (
  fetcher: Fetcher,
  createTimeoutSignal: (milliseconds: number) => AbortSignal,
  url: URL,
  init: RequestInit,
  code: string,
): Promise<Response> => {
  try {
    return await fetcher(url, { ...init, redirect: "manual", signal: createTimeoutSignal(10_000) });
  } catch {
    throw new SentinelIncidentDeliveryError(code);
  }
};

const githubHeaders = (token: string): Headers =>
  new Headers({
    Accept: "application/vnd.github+json",
    Authorization: `Bearer ${token}`,
    "Content-Type": "application/json",
    "X-GitHub-Api-Version": GITHUB_API_VERSION,
  });

const createGitHubAppClient = async (
  privateKeyPem: string,
  dependencies: SentinelIncidentDependencies,
): Promise<GitHubAppClient> => {
  const now = dependencies.now?.() ?? Date.now();
  const fetcher = dependencies.fetcher ?? fetch;
  const createTimeoutSignal = dependencies.createTimeoutSignal ?? AbortSignal.timeout;
  const jwt = await createSentinelGitHubAppJwt(privateKeyPem, now);
  const tokenUrl = new URL(`app/installations/${GITHUB_INSTALLATION_ID}/access_tokens`, GITHUB_API_BASE);
  const tokenResponse = await request(fetcher, createTimeoutSignal, tokenUrl, {
    method: "POST",
    headers: githubHeaders(jwt),
    body: JSON.stringify({ repositories: ["ai.ubq.fi"], permissions: { actions: "write" } }),
  }, "github_app_token_request_failed");
  if (tokenResponse.status !== 201) {
    await tokenResponse.body?.cancel().catch(() => {});
    throw new SentinelIncidentDeliveryError(`github_app_token_http_${tokenResponse.status}`);
  }
  const tokenPayload = await responseJson(tokenResponse, "github_app_token_invalid_json");
  const token = typeof tokenPayload.token === "string" ? tokenPayload.token.trim() : "";
  const expiresAt = typeof tokenPayload.expires_at === "string" ? Date.parse(tokenPayload.expires_at) : NaN;
  if (!token || token.length > 8_192 || !Number.isFinite(expiresAt) || expiresAt <= now + 60_000) {
    throw new SentinelIncidentDeliveryError("github_app_token_invalid");
  }

  return {
    async dispatch(incident) {
      const dispatchUrl = new URL(
        `repos/${GITHUB_REPOSITORY}/actions/workflows/${GITHUB_WORKFLOW}/dispatches`,
        GITHUB_API_BASE,
      );
      const response = await request(fetcher, createTimeoutSignal, dispatchUrl, {
        method: "POST",
        headers: githubHeaders(token),
        body: JSON.stringify({
          ref: GITHUB_REF,
          inputs: {
            sentinel_mode: "incident",
            incident_id: incident.id,
            incident_attempt: String(incident.attempt),
            incident_start_ms: String(incident.first_observed_at_ms),
            incident_ack_nonce: incident.ack_nonce,
          },
          return_run_details: true,
        }),
      }, "github_workflow_dispatch_request_failed");
      if (response.status !== 200) {
        await response.body?.cancel().catch(() => {});
        throw new SentinelIncidentDeliveryError(`github_workflow_dispatch_http_${response.status}`);
      }
      const payload = await responseJson(response, "github_workflow_dispatch_invalid_json");
      const runId = positiveInteger(payload.workflow_run_id) ? payload.workflow_run_id : null;
      const runUrl = typeof payload.run_url === "string" ? payload.run_url : null;
      const htmlUrl = typeof payload.html_url === "string" ? payload.html_url : null;
      if (
        !runId || runUrl !== `https://api.github.com/repos/${GITHUB_REPOSITORY}/actions/runs/${runId}` ||
        htmlUrl !== `https://github.com/${GITHUB_REPOSITORY}/actions/runs/${runId}`
      ) throw new SentinelIncidentDeliveryError("github_workflow_dispatch_invalid_run");
      return { runId, htmlUrl };
    },
    async getRun(runId) {
      if (!positiveInteger(runId)) throw new SentinelIncidentDeliveryError("github_workflow_run_id_invalid");
      const runUrl = new URL(`repos/${GITHUB_REPOSITORY}/actions/runs/${runId}`, GITHUB_API_BASE);
      const response = await request(fetcher, createTimeoutSignal, runUrl, {
        method: "GET",
        headers: githubHeaders(token),
      }, "github_workflow_run_request_failed");
      if (response.status !== 200) {
        await response.body?.cancel().catch(() => {});
        throw new SentinelIncidentDeliveryError(`github_workflow_run_http_${response.status}`);
      }
      const payload = await responseJson(response, "github_workflow_run_invalid_json");
      const id = positiveInteger(payload.id) ? payload.id : null;
      const status = typeof payload.status === "string" ? payload.status : "";
      const conclusion = payload.conclusion === null || typeof payload.conclusion === "string"
        ? payload.conclusion as string | null
        : undefined;
      const htmlUrl = typeof payload.html_url === "string" ? payload.html_url : "";
      const headSha = typeof payload.head_sha === "string" ? payload.head_sha : "";
      if (
        id !== runId || !status || conclusion === undefined ||
        htmlUrl !== `https://github.com/${GITHUB_REPOSITORY}/actions/runs/${runId}` || !FULL_SHA.test(headSha)
      ) throw new SentinelIncidentDeliveryError("github_workflow_run_invalid");
      return { id, status, conclusion, htmlUrl, headSha };
    },
  };
};

const updateActive = async (
  kv: Deno.Kv,
  incidentId: string,
  incidentAttempt: number,
  update: (active: SentinelIncidentBatch, control: SentinelIncidentControl) => SentinelIncidentControl,
): Promise<boolean> => {
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const entry = await controlEntry(kv);
    const control = entry.value;
    if (!control?.active || control.active.id !== incidentId || control.active.attempt !== incidentAttempt) {
      return false;
    }
    const committed = await setControl(kv, entry, update(control.active, control));
    if (committed.ok) return true;
  }
  throw new Error("Sentinel incident active update conflicted repeatedly");
};

const dispatchBackoffMs = (attempt: number): number => Math.min(15 * 60_000, 60_000 * 2 ** Math.max(0, attempt - 1));

const infrastructureDeferralBackoffMs = (deferral: number): number =>
  Math.min(60 * 60_000, 5 * 60_000 * 2 ** Math.max(0, deferral - 1));

const rotatedAckNonce = (previous: string, randomAckNonce: () => string): string => {
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const candidate = randomAckNonce();
    if (!ACK_NONCE.test(candidate)) throw new Error("Sentinel incident ACK nonce is invalid");
    if (candidate !== previous) return candidate;
  }
  throw new Error("Sentinel incident ACK nonce did not rotate");
};

const claimDispatch = async (
  kv: Deno.Kv,
  now: number,
): Promise<SentinelIncidentBatch | null> => {
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const entry = await controlEntry(kv);
    const control = entry.value;
    const active = control?.active;
    if (!control || !active) return null;
    const due = active.state === "queued"
      ? active.next_action_at_ms <= now
      : active.state === "dispatching" && (active.lease_expires_at_ms ?? Number.MAX_SAFE_INTEGER) <= now;
    if (!due) return null;
    const claimed: SentinelIncidentBatch = {
      ...active,
      state: "dispatching",
      next_action_at_ms: now,
      lease_expires_at_ms: now + DISPATCH_LEASE_MS,
      workflow_run_id: null,
      workflow_run_url: null,
      success_observed_at_ms: null,
    };
    const committed = await setControl(kv, entry, { ...control, active: claimed, updated_at_ms: now });
    if (committed.ok) return claimed;
  }
  throw new Error("Sentinel incident dispatch claim conflicted repeatedly");
};

const claimWorkflowPoll = async (
  kv: Deno.Kv,
  now: number,
): Promise<SentinelIncidentBatch | null> => {
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const entry = await controlEntry(kv);
    const control = entry.value;
    const active = control?.active;
    if (!control || !active || active.state !== "dispatched" || active.next_action_at_ms > now) return null;
    const claimed = { ...active, next_action_at_ms: now + WORKFLOW_POLL_MS };
    const committed = await setControl(kv, entry, { ...control, active: claimed, updated_at_ms: now });
    if (committed.ok) return claimed;
  }
  throw new Error("Sentinel incident workflow poll conflicted repeatedly");
};

const promotePending = (control: SentinelIncidentControl, now: number): SentinelIncidentControl => ({
  version: 1,
  active: control.pending ? { ...control.pending, next_action_at_ms: now } : null,
  pending: null,
  updated_at_ms: now,
});

const failConfirmedWorkflow = async (
  kv: Deno.Kv,
  batch: SentinelIncidentBatch,
  now: number,
  conclusion: string,
  randomAckNonce: () => string = defaultAckNonce,
): Promise<"retry" | "dead_letter" | "stale"> => {
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const entry = await controlEntry(kv);
    const control = entry.value;
    const active = control?.active;
    if (
      !control || !active || active.id !== batch.id || active.attempt !== batch.attempt ||
      active.state !== "dispatched" || active.workflow_run_id !== batch.workflow_run_id ||
      active.ack_nonce !== batch.ack_nonce
    ) return "stale";
    const operation = kv.atomic().check({ key: SENTINEL_INCIDENT_CONTROL_KEY, versionstamp: entry.versionstamp });
    if (active.attempt < MAX_WORKFLOW_ATTEMPTS) {
      const ackNonce = randomAckNonce();
      if (!ACK_NONCE.test(ackNonce)) throw new Error("Sentinel incident ACK nonce is invalid");
      const queued: SentinelIncidentBatch = {
        ...active,
        state: "queued",
        attempt: active.attempt + 1,
        next_action_at_ms: now + dispatchBackoffMs(active.attempt + 1),
        lease_expires_at_ms: null,
        workflow_run_id: null,
        workflow_run_url: null,
        success_observed_at_ms: null,
        ack_nonce: ackNonce,
      };
      const committed = await operation
        .set(SENTINEL_INCIDENT_CONTROL_KEY, { ...control, active: queued, updated_at_ms: now }, {
          expireIn: SENTINEL_INCIDENT_TTL_MS,
        })
        .commit();
      if (committed.ok) return "retry";
    } else {
      const dead = {
        version: 1,
        incident_id: active.id,
        attempt: active.attempt,
        workflow_run_id: active.workflow_run_id,
        conclusion: /^[a-z_]{1,64}$/.test(conclusion) ? conclusion : "unknown",
        recorded_at_ms: now,
      };
      const committed = await operation
        .set(SENTINEL_INCIDENT_CONTROL_KEY, promotePending(control, now), { expireIn: SENTINEL_INCIDENT_TTL_MS })
        .set([...SENTINEL_INCIDENT_DEAD_PREFIX, active.id], dead, { expireIn: SENTINEL_INCIDENT_TTL_MS })
        .commit();
      if (committed.ok) return "dead_letter";
    }
  }
  throw new Error("Sentinel incident retry update conflicted repeatedly");
};

export type SentinelIncidentReconcileResult = Readonly<{
  status: "idle" | "waiting" | "dispatched" | "running" | "retry" | "dead_letter" | "deferred";
  incidentId?: string;
  attempt?: number;
  workflowRunId?: number;
  reason?: string;
}>;

export const reconcileSentinelIncidentOutbox = async (
  kv: Deno.Kv,
  privateKeyPem: string,
  dependencies: SentinelIncidentDependencies = {},
): Promise<SentinelIncidentReconcileResult> => {
  const now = dependencies.now?.() ?? Date.now();
  if (!positiveInteger(now)) throw new Error("Sentinel incident clock is invalid");
  await coalesceSentinelIncidentFailureEvents(kv, now, dependencies);
  const dispatch = await claimDispatch(kv, now);
  if (dispatch) {
    try {
      const client = await createGitHubAppClient(privateKeyPem, dependencies);
      const run = await client.dispatch(dispatch);
      await claimSentinelIncidentWorkflowRun(kv, {
        incidentId: dispatch.id,
        attempt: dispatch.attempt,
        workflowRunId: run.runId,
        ackNonce: dispatch.ack_nonce,
      }, now);
      return { status: "dispatched", incidentId: dispatch.id, attempt: dispatch.attempt, workflowRunId: run.runId };
    } catch (error) {
      const reason = error instanceof SentinelIncidentDeliveryError ? error.code : "github_dispatch_failed";
      await updateActive(
        kv,
        dispatch.id,
        dispatch.attempt,
        (active, control) =>
          active.state === "dispatched" ? control : {
            ...control,
            active: {
              ...active,
              state: "dispatching",
              next_action_at_ms: now + dispatchBackoffMs(active.attempt),
              lease_expires_at_ms: now + dispatchBackoffMs(active.attempt),
              workflow_run_id: null,
              workflow_run_url: null,
              success_observed_at_ms: null,
            },
            updated_at_ms: now,
          },
      );
      return { status: "deferred", incidentId: dispatch.id, attempt: dispatch.attempt, reason };
    }
  }

  const poll = await claimWorkflowPoll(kv, now);
  if (!poll) {
    const entry = await controlEntry(kv);
    return { status: entry.value?.active ? "waiting" : "idle" };
  }
  try {
    const client = await createGitHubAppClient(privateKeyPem, dependencies);
    const run = await client.getRun(poll.workflow_run_id!);
    if (run.status !== "completed" || run.conclusion === null) {
      return { status: "running", incidentId: poll.id, attempt: poll.attempt, workflowRunId: run.id };
    }
    if (run.conclusion === "success") {
      if (poll.success_observed_at_ms === null) {
        await updateActive(kv, poll.id, poll.attempt, (active, control) => ({
          ...control,
          active: {
            ...active,
            success_observed_at_ms: now,
            next_action_at_ms: now + WORKFLOW_ACK_GRACE_MS,
          },
          updated_at_ms: now,
        }));
        return { status: "running", incidentId: poll.id, attempt: poll.attempt, workflowRunId: run.id };
      }
      if (now < poll.success_observed_at_ms + WORKFLOW_ACK_GRACE_MS) {
        return { status: "running", incidentId: poll.id, attempt: poll.attempt, workflowRunId: run.id };
      }
      const disposition = await failConfirmedWorkflow(
        kv,
        poll,
        now,
        "success_without_ack",
        dependencies.randomAckNonce,
      );
      return {
        status: disposition === "dead_letter" ? "dead_letter" : disposition === "retry" ? "retry" : "waiting",
        incidentId: poll.id,
        attempt: poll.attempt,
        workflowRunId: run.id,
      };
    }
    const disposition = await failConfirmedWorkflow(kv, poll, now, run.conclusion, dependencies.randomAckNonce);
    return {
      status: disposition === "dead_letter" ? "dead_letter" : disposition === "retry" ? "retry" : "waiting",
      incidentId: poll.id,
      attempt: poll.attempt,
      workflowRunId: run.id,
    };
  } catch (error) {
    return {
      status: "deferred",
      incidentId: poll.id,
      attempt: poll.attempt,
      workflowRunId: poll.workflow_run_id ?? undefined,
      reason: error instanceof SentinelIncidentDeliveryError ? error.code : "github_workflow_poll_failed",
    };
  }
};

type SentinelIncidentWorkflowIdentity = Readonly<{
  incidentId: string;
  attempt: number;
  workflowRunId: number;
  ackNonce: string;
}>;

type SentinelIncidentDeferralIdentity =
  & SentinelIncidentWorkflowIdentity
  & Readonly<{
    reason: SentinelIncidentDeferralReason;
  }>;

const validateWorkflowIdentity = (input: SentinelIncidentWorkflowIdentity, now: number): void => {
  if (
    !isSentinelIncidentId(input.incidentId) || !positiveInteger(input.attempt) ||
    input.attempt > MAX_WORKFLOW_ATTEMPTS || !positiveInteger(input.workflowRunId) ||
    typeof input.ackNonce !== "string" || !ACK_NONCE.test(input.ackNonce) || !positiveInteger(now)
  ) throw new Error("Sentinel incident workflow identity is invalid");
};

export const claimSentinelIncidentWorkflowRun = async (
  kv: Deno.Kv,
  input: SentinelIncidentWorkflowIdentity,
  now = Date.now(),
): Promise<"claimed" | "duplicate"> => {
  validateWorkflowIdentity(input, now);
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const entry = await controlEntry(kv);
    const control = entry.value;
    const active = control?.active;
    if (
      !control || !active || active.id !== input.incidentId || active.attempt !== input.attempt ||
      active.ack_nonce !== input.ackNonce
    ) throw new SentinelIncidentClaimConflict();
    if (active.state === "dispatched") {
      if (active.workflow_run_id !== input.workflowRunId) throw new SentinelIncidentClaimConflict();
      return "duplicate";
    }
    if (active.state !== "dispatching") throw new SentinelIncidentClaimConflict();
    const claimed: SentinelIncidentBatch = {
      ...active,
      state: "dispatched",
      next_action_at_ms: now + WORKFLOW_POLL_MS,
      lease_expires_at_ms: null,
      workflow_run_id: input.workflowRunId,
      workflow_run_url: `https://github.com/${GITHUB_REPOSITORY}/actions/runs/${input.workflowRunId}`,
      success_observed_at_ms: null,
    };
    const committed = await setControl(kv, entry, { ...control, active: claimed, updated_at_ms: now });
    if (committed.ok) return "claimed";
  }
  throw new Error("Sentinel incident workflow claim conflicted repeatedly");
};

export const deferSentinelIncident = async (
  kv: Deno.Kv,
  input: SentinelIncidentDeferralIdentity,
  now = Date.now(),
  dependencies: Pick<SentinelIncidentDependencies, "randomAckNonce"> = {},
): Promise<"deferred" | "dead_letter"> => {
  validateWorkflowIdentity(input, now);
  if (!isSentinelIncidentDeferralReason(input.reason)) {
    throw new Error("Sentinel incident deferral reason is invalid");
  }
  const receiptKey = [
    ...SENTINEL_INCIDENT_DEFER_PREFIX,
    input.incidentId,
    input.attempt,
    input.workflowRunId,
  ] as const;
  const randomAckNonce = dependencies.randomAckNonce ?? defaultAckNonce;
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const [receipt, entry] = await Promise.all([
      kv.get(receiptKey),
      controlEntry(kv),
    ]);
    if (receipt.value !== null) {
      if (
        !isRecord(receipt.value) || receipt.value.version !== 1 ||
        receipt.value.incident_id !== input.incidentId || receipt.value.attempt !== input.attempt ||
        receipt.value.workflow_run_id !== input.workflowRunId || receipt.value.ack_nonce !== input.ackNonce ||
        receipt.value.reason !== input.reason ||
        (receipt.value.disposition !== "deferred" && receipt.value.disposition !== "dead_letter")
      ) throw new SentinelIncidentDeferConflict();
      return receipt.value.disposition;
    }
    const control = entry.value;
    const active = control?.active;
    if (
      !control || !active || active.id !== input.incidentId || active.attempt !== input.attempt ||
      active.state !== "dispatched" || active.workflow_run_id !== input.workflowRunId ||
      active.ack_nonce !== input.ackNonce
    ) throw new SentinelIncidentDeferConflict();

    const infrastructureDeferrals = (active.infrastructure_deferrals ?? 0) + 1;
    const disposition = infrastructureDeferrals >= MAX_SENTINEL_INFRASTRUCTURE_DEFERRALS ? "dead_letter" : "deferred";
    const receiptValue = {
      version: 1,
      incident_id: input.incidentId,
      attempt: input.attempt,
      workflow_run_id: input.workflowRunId,
      ack_nonce: input.ackNonce,
      reason: input.reason,
      disposition,
      infrastructure_deferrals: infrastructureDeferrals,
      recorded_at_ms: now,
    } as const;
    let operation = kv.atomic()
      .check({ key: SENTINEL_INCIDENT_CONTROL_KEY, versionstamp: entry.versionstamp })
      .check({ key: receiptKey, versionstamp: receipt.versionstamp })
      .set(receiptKey, receiptValue, { expireIn: SENTINEL_INCIDENT_TTL_MS });
    if (disposition === "deferred") {
      const queued: SentinelIncidentBatch = {
        ...active,
        state: "queued",
        next_action_at_ms: now + infrastructureDeferralBackoffMs(infrastructureDeferrals),
        lease_expires_at_ms: null,
        workflow_run_id: null,
        workflow_run_url: null,
        success_observed_at_ms: null,
        ack_nonce: rotatedAckNonce(active.ack_nonce, randomAckNonce),
        infrastructure_deferrals: infrastructureDeferrals,
      };
      operation = operation.set(
        SENTINEL_INCIDENT_CONTROL_KEY,
        { ...control, active: queued, updated_at_ms: now },
        { expireIn: SENTINEL_INCIDENT_TTL_MS },
      );
    } else {
      operation = operation
        .set(SENTINEL_INCIDENT_CONTROL_KEY, promotePending(control, now), { expireIn: SENTINEL_INCIDENT_TTL_MS })
        .set([...SENTINEL_INCIDENT_DEAD_PREFIX, active.id], {
          version: 1,
          incident_id: active.id,
          attempt: active.attempt,
          workflow_run_id: active.workflow_run_id,
          conclusion: "infrastructure_deferrals_exhausted",
          infrastructure_deferrals: infrastructureDeferrals,
          recorded_at_ms: now,
        }, { expireIn: SENTINEL_INCIDENT_TTL_MS });
    }
    const committed = await operation.commit();
    if (committed.ok) return disposition;
  }
  throw new Error("Sentinel incident deferral conflicted repeatedly");
};

export const acknowledgeSentinelIncident = async (
  kv: Deno.Kv,
  input: Readonly<{ incidentId: string; attempt: number; workflowRunId: number; ackNonce: string }>,
  now = Date.now(),
): Promise<"acknowledged" | "duplicate"> => {
  validateWorkflowIdentity(input, now);
  const receiptKey = [...SENTINEL_INCIDENT_ACK_PREFIX, input.incidentId, input.attempt] as const;
  for (let casAttempt = 0; casAttempt < MAX_CAS_ATTEMPTS; casAttempt += 1) {
    const [receipt, entry] = await Promise.all([
      kv.get(receiptKey),
      controlEntry(kv),
    ]);
    if (receipt.value !== null) {
      if (
        !isRecord(receipt.value) || receipt.value.version !== 1 || receipt.value.incident_id !== input.incidentId ||
        receipt.value.attempt !== input.attempt || receipt.value.workflow_run_id !== input.workflowRunId ||
        receipt.value.ack_nonce !== input.ackNonce
      ) throw new SentinelIncidentAckConflict();
      return "duplicate";
    }
    const control = entry.value;
    const active = control?.active;
    if (
      !control || !active || active.id !== input.incidentId || active.attempt !== input.attempt ||
      (active.state !== "dispatching" && active.state !== "dispatched") || active.ack_nonce !== input.ackNonce ||
      (active.state === "dispatched" && active.workflow_run_id !== input.workflowRunId)
    ) {
      throw new SentinelIncidentAckConflict();
    }
    const committed = await kv.atomic()
      .check({ key: SENTINEL_INCIDENT_CONTROL_KEY, versionstamp: entry.versionstamp })
      .check({ key: receiptKey, versionstamp: receipt.versionstamp })
      .set(SENTINEL_INCIDENT_CONTROL_KEY, promotePending(control, now), { expireIn: SENTINEL_INCIDENT_TTL_MS })
      .set(receiptKey, {
        version: 1,
        incident_id: input.incidentId,
        attempt: input.attempt,
        workflow_run_id: input.workflowRunId,
        ack_nonce: input.ackNonce,
        acknowledged_at_ms: now,
      }, { expireIn: SENTINEL_INCIDENT_TTL_MS })
      .commit();
    if (committed.ok) return "acknowledged";
  }
  throw new Error("Sentinel incident acknowledgement conflicted repeatedly");
};

export const recordSentinelIncidentFailureFromEnvironment = async (
  kv: Deno.Kv,
  observedAtMs: number,
  environment: EnvironmentReader = safeEnvironment,
): Promise<Readonly<{ incidentId: string; created: boolean }> | null> =>
  isSentinelProductionRuntime(environment) ? await recordSentinelIncidentFailure(kv, observedAtMs) : null;

export const createSentinelIncidentFailureEventFromEnvironment = async (
  kv: Deno.Kv,
  observedAtMs: number,
  environment: EnvironmentReader = safeEnvironment,
): Promise<Deno.KvEntry<SentinelIncidentFailureEvent> | null> =>
  isSentinelProductionRuntime(environment) ? await createSentinelIncidentFailureEvent(kv, observedAtMs) : null;

export const recordSentinelProviderDegradationFromEnvironment = async (
  observedAtMs: number,
  dependencies: Readonly<{
    environment?: EnvironmentReader;
    kv?: Deno.Kv;
    randomUuid?: () => string;
  }> = {},
): Promise<boolean> => {
  const environment = dependencies.environment ?? safeEnvironment;
  if (!isSentinelProductionRuntime(environment)) return false;
  const kv = dependencies.kv ?? await getKv();
  if (!kv) return false;
  await recordSentinelProviderDegradation(kv, observedAtMs, dependencies);
  return true;
};

const privateKeyFromEnvironment = (environment: EnvironmentReader): string | null =>
  environment.get("SENTINEL_GITHUB_APP_PRIVATE_KEY")?.trim() || null;

export const reconcileSentinelIncidentOutboxFromEnvironment = async (
  dependencies: SentinelIncidentDependencies & Readonly<{ environment?: EnvironmentReader; kv?: Deno.Kv }> = {},
): Promise<SentinelIncidentReconcileResult> => {
  const environment = dependencies.environment ?? safeEnvironment;
  if (!isSentinelProductionRuntime(environment)) return { status: "idle" };
  const kv = dependencies.kv ?? await getKv();
  if (!kv) return { status: "deferred", reason: "kv_unavailable" };
  const now = dependencies.now?.() ?? Date.now();
  await coalesceSentinelIncidentFailureEvents(kv, now, dependencies);
  const entry = await controlEntry(kv);
  if (!entry.value?.active) return { status: "idle" };
  const privateKeyPem = privateKeyFromEnvironment(environment);
  if (!privateKeyPem) return { status: "deferred", incidentId: entry.value.active.id, reason: "credential_missing" };
  return await reconcileSentinelIncidentOutbox(kv, privateKeyPem, dependencies);
};

export const logSentinelIncidentReconcileResult = (result: SentinelIncidentReconcileResult): void => {
  if (result.status === "idle" || result.status === "waiting" || result.status === "running") return;
  const detail = JSON.stringify({
    status: result.status,
    incident_id: result.incidentId ?? null,
    attempt: result.attempt ?? null,
    workflow_run_id: result.workflowRunId ?? null,
    reason: result.reason ?? null,
  });
  if (result.status === "deferred" || result.status === "dead_letter") {
    console.warn("[ai.ubq.fi] sentinel_incident", detail);
  } else {
    console.info("[ai.ubq.fi] sentinel_incident", detail);
  }
};
