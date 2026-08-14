import { getKv } from "./kv.ts";
import { isRecord } from "./utils.ts";

export const CODEX_RATE_LIMIT_KV_KEY = ["uos_ai", "codex_rate_limit"] as const;
export const CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS = 60_000;
export const CODEX_RATE_LIMIT_PROBE_LEASE_MS = 60_000;

export type CodexRateLimitState = Readonly<{
  observed_at_ms: number;
  retry_at_ms: number;
  probe_id?: string;
  probe_lease_until_ms?: number;
}>;

export type CodexRateLimitDecision =
  | Readonly<{ kind: "primary" }>
  | Readonly<{ kind: "probe"; probeId: string }>
  | Readonly<{ kind: "cached"; retryAtMs: number }>;

const isNonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const parseState = (value: unknown): CodexRateLimitState | null => {
  if (!isRecord(value)) return null;
  if (!isNonNegativeSafeInteger(value.observed_at_ms) || !isNonNegativeSafeInteger(value.retry_at_ms)) return null;
  const hasProbeId = Object.prototype.hasOwnProperty.call(value, "probe_id");
  const hasProbeLease = Object.prototype.hasOwnProperty.call(value, "probe_lease_until_ms");
  if (hasProbeId !== hasProbeLease) return null;
  if (!hasProbeId) {
    return { observed_at_ms: value.observed_at_ms, retry_at_ms: value.retry_at_ms };
  }
  if (typeof value.probe_id !== "string" || !value.probe_id || !isNonNegativeSafeInteger(value.probe_lease_until_ms)) {
    return null;
  }
  return {
    observed_at_ms: value.observed_at_ms,
    retry_at_ms: value.retry_at_ms,
    probe_id: value.probe_id,
    probe_lease_until_ms: value.probe_lease_until_ms,
  };
};

export const retryDeadlineFromHeader = (retryAfter: string | null, nowMs: number): number => {
  const value = retryAfter?.trim() ?? "";
  if (/^\d+$/.test(value)) {
    const seconds = Number(value);
    const deadline = nowMs + seconds * 1000;
    if (Number.isSafeInteger(deadline) && deadline > nowMs) return deadline;
  } else if (value) {
    const deadline = Date.parse(value);
    if (Number.isSafeInteger(deadline) && deadline > nowMs) return deadline;
  }
  return nowMs + CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS;
};

export const getCodexRateLimitDecision = async (
  kv: Deno.Kv | null,
  nowMs = Date.now(),
  createProbeId: () => string = () => crypto.randomUUID(),
): Promise<CodexRateLimitDecision> => {
  if (!kv) return { kind: "primary" };
  try {
    for (let attempt = 0; attempt < 5; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY);
      if (entry.value === null) return { kind: "primary" };
      const state = parseState(entry.value);
      if (!state) return { kind: "primary" };
      if (state.retry_at_ms > nowMs) return { kind: "cached", retryAtMs: state.retry_at_ms };
      if (state.probe_id && state.probe_lease_until_ms && state.probe_lease_until_ms > nowMs) {
        return { kind: "cached", retryAtMs: state.probe_lease_until_ms };
      }

      const probeId = createProbeId();
      const probeState: CodexRateLimitState = {
        observed_at_ms: state.observed_at_ms,
        retry_at_ms: state.retry_at_ms,
        probe_id: probeId,
        probe_lease_until_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS,
      };
      const commit = await kv.atomic().check(entry).set(CODEX_RATE_LIMIT_KV_KEY, probeState).commit();
      if (commit.ok) return { kind: "probe", probeId };
    }
    return { kind: "cached", retryAtMs: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS };
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit circuit read failed open:", error);
    return { kind: "primary" };
  }
};

export const getGlobalCodexRateLimitDecision = async (nowMs = Date.now()): Promise<CodexRateLimitDecision> =>
  await getCodexRateLimitDecision(await getKv(), nowMs);

export const openCodexRateLimitCircuit = async (
  kv: Deno.Kv | null,
  retryAfter: string | null,
  nowMs = Date.now(),
): Promise<number> => {
  const requestedRetryAtMs = retryDeadlineFromHeader(retryAfter, nowMs);
  if (!kv) return requestedRetryAtMs;
  try {
    for (let attempt = 0; attempt < 5; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY);
      const state = parseState(entry.value);
      const retryAtMs = Math.max(requestedRetryAtMs, state?.retry_at_ms ?? 0);
      const next: CodexRateLimitState = { observed_at_ms: nowMs, retry_at_ms: retryAtMs };
      const commit = await kv.atomic().check(entry).set(CODEX_RATE_LIMIT_KV_KEY, next).commit();
      if (commit.ok) return retryAtMs;
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit circuit update failed:", error);
  }
  return requestedRetryAtMs;
};

export const openGlobalCodexRateLimitCircuit = async (
  retryAfter: string | null,
  nowMs = Date.now(),
): Promise<number> => await openCodexRateLimitCircuit(await getKv(), retryAfter, nowMs);

export const closeCodexRateLimitProbe = async (kv: Deno.Kv | null, probeId: string): Promise<void> => {
  if (!kv) return;
  try {
    for (let attempt = 0; attempt < 5; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY);
      const state = parseState(entry.value);
      if (!state || state.probe_id !== probeId) return;
      const commit = await kv.atomic().check(entry).delete(CODEX_RATE_LIMIT_KV_KEY).commit();
      if (commit.ok) return;
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit probe close failed:", error);
  }
};

export const closeGlobalCodexRateLimitProbe = async (probeId: string): Promise<void> =>
  await closeCodexRateLimitProbe(await getKv(), probeId);
