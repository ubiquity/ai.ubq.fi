import { getKv } from "./kv.ts";
import { isRecord } from "./utils.ts";

export const PROVIDER_HEALTH_KEY_PREFIX = ["uos_ai", "provider_health", "v1"] as const;
export const PROVIDER_HEALTH_SUCCESS_WRITE_INTERVAL_MS = 60_000;
export const PROVIDER_HEALTH_STALE_AFTER_MS = 30 * 60_000;

export type ProviderHealthState = "healthy" | "degraded" | "exhausted" | "invalid" | "unknown";
export type ProviderHealthEvent =
  | "success"
  | "reachable"
  | "auth_invalid"
  | "quota_exhausted"
  | "upstream_error"
  | "refresh_success"
  | "refresh_failed";

export type ProviderHealthObservation = Readonly<{
  event: ProviderHealthEvent;
  status: number | null;
  observed_at_ms: number;
}>;

export type ProviderHealthView = Readonly<{
  state: ProviderHealthState;
  stale: boolean | null;
  last_event: ProviderHealthEvent | null;
  last_status: number | null;
  last_observed_at_ms: number | null;
  last_success_at_ms: number | null;
  last_401_at_ms: number | null;
  last_429_at_ms: number | null;
  last_error_at_ms: number | null;
  last_refresh_at_ms: number | null;
  last_refresh_succeeded: boolean | null;
}>;

type RecordProvider = "codex" | "yunwu";

const PROVIDER_RECORDS = ["current", "refresh_success"] as const;

const lastSuccessWriteAtMs = new Map<string, number>();

const providerHealthKey = (
  provider: RecordProvider,
  identity: string,
  record: (typeof PROVIDER_RECORDS)[number],
): Deno.KvKey => [...PROVIDER_HEALTH_KEY_PREFIX, provider, identity, record];

const isProviderEvent = (value: unknown): value is ProviderHealthEvent =>
  value === "success" ||
  value === "reachable" ||
  value === "auth_invalid" ||
  value === "quota_exhausted" ||
  value === "upstream_error" ||
  value === "refresh_success" ||
  value === "refresh_failed";

export const parseProviderHealthObservation = (value: unknown): ProviderHealthObservation | null => {
  if (
    !isRecord(value) ||
    !isProviderEvent(value.event) ||
    !(value.status === null || (typeof value.status === "number" && Number.isFinite(value.status))) ||
    !(typeof value.observed_at_ms === "number" && Number.isFinite(value.observed_at_ms) && value.observed_at_ms >= 0)
  ) {
    return null;
  }
  return value as ProviderHealthObservation;
};

const stateForEvent = (event: ProviderHealthEvent): Exclude<ProviderHealthState, "unknown"> => {
  if (event === "success" || event === "reachable" || event === "refresh_success") return "healthy";
  if (event === "auth_invalid" || event === "refresh_failed") return "invalid";
  if (event === "quota_exhausted") return "exhausted";
  return "degraded";
};

const shouldThrottleSuccess = (
  provider: RecordProvider,
  identity: string,
  event: ProviderHealthEvent,
  nowMs: number,
): boolean => {
  if (event !== "success" && event !== "reachable") return false;
  const throttleKey = `${provider}:${identity}`;
  const previous = lastSuccessWriteAtMs.get(throttleKey);
  return previous !== undefined && nowMs - previous < PROVIDER_HEALTH_SUCCESS_WRITE_INTERVAL_MS;
};

const recordProviderHealth = async (
  provider: RecordProvider,
  identity: string,
  event: ProviderHealthEvent,
  status: number | null,
  now: () => number,
): Promise<void> => {
  try {
    const observedAtMs = Math.trunc(now());
    if (shouldThrottleSuccess(provider, identity, event, observedAtMs)) return;
    const kv = await getKv();
    if (!kv) return;
    const commit = await kv.set(
      providerHealthKey(provider, identity, event === "refresh_success" ? "refresh_success" : "current"),
      {
        event,
        status,
        observed_at_ms: observedAtMs,
      } satisfies ProviderHealthObservation,
    );
    if (!commit.ok) return;
    if (event === "success" || event === "reachable") {
      lastSuccessWriteAtMs.set(`${provider}:${identity}`, observedAtMs);
    }
  } catch {
    // Health observations are optional telemetry and must never affect routing.
  }
};

export const recordCodexProviderHealth = (
  accountId: string,
  event: ProviderHealthEvent,
  status: number | null = null,
  now: () => number = Date.now,
): Promise<void> => recordProviderHealth("codex", accountId, event, status, now);

export const recordYunwuProviderHealth = (
  event: ProviderHealthEvent,
  status: number | null = null,
  now: () => number = Date.now,
): Promise<void> => recordProviderHealth("yunwu", "default", event, status, now);

const unknownView = (): ProviderHealthView => ({
  state: "unknown",
  stale: null,
  last_event: null,
  last_status: null,
  last_observed_at_ms: null,
  last_success_at_ms: null,
  last_401_at_ms: null,
  last_429_at_ms: null,
  last_error_at_ms: null,
  last_refresh_at_ms: null,
  last_refresh_succeeded: null,
});

const latestObservation = (
  observations: readonly ProviderHealthObservation[],
  events?: readonly ProviderHealthEvent[],
): ProviderHealthObservation | null => {
  const allowed = events ? new Set(events) : null;
  return observations
    .filter((observation) => !allowed || allowed.has(observation.event))
    .reduce<ProviderHealthObservation | null>(
      (latest, observation) => !latest || observation.observed_at_ms > latest.observed_at_ms ? observation : latest,
      null,
    );
};

const toView = (
  observations: readonly ProviderHealthObservation[],
  nowMs: number,
): ProviderHealthView => {
  const latest = latestObservation(observations);
  if (!latest) return unknownView();
  const latestState = latestObservation(observations, [
    "success",
    "reachable",
    "auth_invalid",
    "quota_exhausted",
    "upstream_error",
    "refresh_failed",
  ]);
  const success = latestObservation(observations, ["success", "reachable"]);
  const authInvalid = latestObservation(observations, ["auth_invalid"]);
  const exhausted = latestObservation(observations, ["quota_exhausted"]);
  const error = latestObservation(observations, ["upstream_error", "refresh_failed"]);
  const refresh = latestObservation(observations, ["refresh_success", "refresh_failed"]);
  return {
    state: latestState ? stateForEvent(latestState.event) : "unknown",
    stale: nowMs - latest.observed_at_ms > PROVIDER_HEALTH_STALE_AFTER_MS,
    last_event: latest.event,
    last_status: latest.status,
    last_observed_at_ms: latest.observed_at_ms,
    last_success_at_ms: success?.observed_at_ms ?? null,
    last_401_at_ms: authInvalid?.observed_at_ms ?? null,
    last_429_at_ms: exhausted?.observed_at_ms ?? null,
    last_error_at_ms: error?.observed_at_ms ?? null,
    last_refresh_at_ms: refresh?.observed_at_ms ?? null,
    last_refresh_succeeded: refresh ? refresh.event === "refresh_success" : null,
  };
};

const readProviderHealth = async (
  provider: RecordProvider,
  identity: string,
  now: () => number,
): Promise<ProviderHealthView> => {
  const kv = await getKv();
  if (!kv) return unknownView();
  const entries = await Promise.all(
    PROVIDER_RECORDS.map((record) => kv.get<ProviderHealthObservation>(providerHealthKey(provider, identity, record))),
  );
  const observations = entries
    .map((entry) => parseProviderHealthObservation(entry.value))
    .filter((value): value is ProviderHealthObservation => value !== null);
  return toView(observations, Math.trunc(now()));
};

export const getCodexProviderHealth = (
  accountId: string,
  now: () => number = Date.now,
): Promise<ProviderHealthView> => readProviderHealth("codex", accountId, now);

export const getYunwuProviderHealth = (
  now: () => number = Date.now,
): Promise<ProviderHealthView> => readProviderHealth("yunwu", "default", now);

export const resetProviderHealthThrottleForTest = (): void => {
  lastSuccessWriteAtMs.clear();
};
