import { kvPromise } from "./kv.ts";
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

const validInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const parseState = (value: unknown): CodexRateLimitState | null => {
  if (!isRecord(value) || !validInteger(value.observed_at_ms) || !validInteger(value.retry_at_ms)) return null;
  const probeId = typeof value.probe_id === "string" && value.probe_id ? value.probe_id : null;
  const probeLease = validInteger(value.probe_lease_until_ms) ? value.probe_lease_until_ms : null;
  if ((probeId === null) !== (probeLease === null)) return null;
  return probeId
    ? {
      observed_at_ms: value.observed_at_ms,
      retry_at_ms: value.retry_at_ms,
      probe_id: probeId,
      probe_lease_until_ms: probeLease!,
    }
    : { observed_at_ms: value.observed_at_ms, retry_at_ms: value.retry_at_ms };
};

export const retryDeadlineFromHeader = (retryAfter: string | null, nowMs: number): number => {
  const value = retryAfter?.trim() ?? "";
  if (/^\d+$/.test(value)) {
    const deadline = nowMs + Number(value) * 1000;
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
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
      const state = parseState(entry.value);
      if (!state) return { kind: "primary" };
      if (state.retry_at_ms > nowMs) return { kind: "cached", retryAtMs: state.retry_at_ms };
      if (state.probe_id && state.probe_lease_until_ms! > nowMs) {
        return { kind: "cached", retryAtMs: state.probe_lease_until_ms! };
      }
      const probeId = createProbeId();
      const next: CodexRateLimitState = {
        observed_at_ms: state.observed_at_ms,
        retry_at_ms: state.retry_at_ms,
        probe_id: probeId,
        probe_lease_until_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS,
      };
      if ((await kv.atomic().check(entry).set(CODEX_RATE_LIMIT_KV_KEY, next).commit()).ok) {
        return { kind: "probe", probeId };
      }
    }
    return { kind: "cached", retryAtMs: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS };
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit circuit read failed open:", error);
    return { kind: "primary" };
  }
};

let localState: CodexRateLimitState | null = null;
let hydrated = false;

export const getGlobalCodexRateLimitDecision = async (nowMs = Date.now()): Promise<CodexRateLimitDecision> => {
  if (!hydrated) {
    const kv = await kvPromise;
    if (!kv) {
      hydrated = true;
      return { kind: "primary" };
    }
    try {
      localState = parseState((await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY)).value);
    } catch (error) {
      console.warn("[ai.ubq.fi] Codex rate-limit circuit hydration failed open:", error);
      localState = null;
    }
    hydrated = true;
  }
  // Deliberately avoid re-reading a healthy/null circuit: the warm success path has a zero-read budget.
  // If another isolate opened the shared circuit, this isolate can leak at most one primary 429; that response
  // immediately calls openGlobalCodexRateLimitCircuit and hydrates this isolate for the rest of the cooldown.
  if (!localState) return { kind: "primary" };
  if (localState.retry_at_ms > nowMs) return { kind: "cached", retryAtMs: localState.retry_at_ms };
  if (localState.probe_id && localState.probe_lease_until_ms! > nowMs) {
    return { kind: "cached", retryAtMs: localState.probe_lease_until_ms! };
  }
  const decision = await getCodexRateLimitDecision(await kvPromise, nowMs);
  if (decision.kind === "cached") {
    localState = { observed_at_ms: nowMs, retry_at_ms: decision.retryAtMs };
  } else if (decision.kind === "probe") {
    localState = {
      observed_at_ms: localState.observed_at_ms,
      retry_at_ms: localState.retry_at_ms,
      probe_id: decision.probeId,
      probe_lease_until_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS,
    };
  } else {
    localState = null;
  }
  return decision;
};

export const openCodexRateLimitCircuit = async (
  kv: Deno.Kv | null,
  retryAfter: string | null,
  nowMs = Date.now(),
): Promise<number> => {
  const requested = retryDeadlineFromHeader(retryAfter, nowMs);
  if (!kv) return requested;
  try {
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
      const retryAtMs = Math.max(requested, parseState(entry.value)?.retry_at_ms ?? 0);
      const next: CodexRateLimitState = { observed_at_ms: nowMs, retry_at_ms: retryAtMs };
      if ((await kv.atomic().check(entry).set(CODEX_RATE_LIMIT_KV_KEY, next).commit()).ok) return retryAtMs;
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit circuit update failed:", error);
  }
  return requested;
};

export const openGlobalCodexRateLimitCircuit = async (
  retryAfter: string | null,
  nowMs = Date.now(),
): Promise<number> => {
  const retryAtMs = await openCodexRateLimitCircuit(await kvPromise, retryAfter, nowMs);
  localState = { observed_at_ms: nowMs, retry_at_ms: retryAtMs };
  hydrated = true;
  return retryAtMs;
};

export const closeCodexRateLimitProbe = async (kv: Deno.Kv | null, probeId: string): Promise<void> => {
  if (!kv) return;
  try {
    const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
    if (parseState(entry.value)?.probe_id !== probeId) return;
    await kv.atomic().check(entry).delete(CODEX_RATE_LIMIT_KV_KEY).commit();
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit probe close failed:", error);
  }
};

export const closeGlobalCodexRateLimitProbe = async (probeId: string): Promise<void> => {
  await closeCodexRateLimitProbe(await kvPromise, probeId);
  if (localState?.probe_id === probeId) localState = null;
};

export const resetCodexRateLimitCacheForTest = (): void => {
  localState = null;
  hydrated = false;
};
