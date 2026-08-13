import { getKv } from "./kv.ts";
import { isRecord } from "./utils.ts";

export const OPENROUTER_CIRCUIT_KEY = ["uos_ai", "openrouter_failover", "circuit", "v1"] as const;
export const OPENROUTER_FAILURE_WINDOW_MS = 60_000;
export const OPENROUTER_FAILURE_THRESHOLD = 2;
export const OPENROUTER_OPEN_MS = 2 * 60_000;
// The lease must outlive the 120-second first-semantic deadline.
export const OPENROUTER_PROBE_LEASE_MS = 150_000;
export const OPENROUTER_CIRCUIT_TTL_MS = 10 * 60_000;
const MAX_CAS_ATTEMPTS = 5;

export type OpenRouterCircuitPhase = "closed" | "open" | "half_open";
export type OpenRouterCircuitTransition = "none" | "opened" | "probe_claimed" | "closed" | "reopened" | "released";
export type OpenRouterCircuitReason = "closed" | "open" | "probe" | "concurrent_probe" | "unavailable";

export type OpenRouterCircuitProbe = Readonly<{
  token: string;
  generation: number;
  lease_until_ms: number;
  source: "expiry" | "early_recovery";
}>;

export type OpenRouterCircuitState = Readonly<{
  v: 1;
  phase: OpenRouterCircuitPhase;
  failure_at_ms: number[];
  open_until_ms: number | null;
  generation: number;
  probe: OpenRouterCircuitProbe | null;
  updated_at_ms: number;
}>;

export type OpenRouterCircuitDecision = Readonly<{
  route: "codex" | "openrouter";
  reason: OpenRouterCircuitReason;
  probe: OpenRouterCircuitProbe | null;
  transition: OpenRouterCircuitTransition;
}>;

const closedState = (nowMs: number, generation = 0): OpenRouterCircuitState => ({
  v: 1,
  phase: "closed",
  failure_at_ms: [],
  open_until_ms: null,
  generation,
  probe: null,
  updated_at_ms: nowMs,
});

const safeMs = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const parseProbe = (value: unknown): OpenRouterCircuitProbe | null => {
  if (
    !isRecord(value) || typeof value.token !== "string" || !value.token || value.token.length > 256 ||
    !safeMs(value.generation) || !safeMs(value.lease_until_ms) ||
    (value.source !== "expiry" && value.source !== "early_recovery")
  ) return null;
  return {
    token: value.token,
    generation: value.generation,
    lease_until_ms: value.lease_until_ms,
    source: value.source,
  };
};

export const parseOpenRouterCircuitState = (value: unknown): OpenRouterCircuitState | null => {
  if (
    !isRecord(value) || value.v !== 1 ||
    (value.phase !== "closed" && value.phase !== "open" && value.phase !== "half_open") ||
    !Array.isArray(value.failure_at_ms) || !value.failure_at_ms.every(safeMs) ||
    !(value.open_until_ms === null || safeMs(value.open_until_ms)) ||
    !safeMs(value.generation) || !safeMs(value.updated_at_ms)
  ) return null;
  const probe = value.probe === null ? null : parseProbe(value.probe);
  if (value.probe !== null && !probe) return null;
  if (value.phase === "closed" && (value.open_until_ms !== null || probe !== null)) return null;
  if (value.phase === "open" && (value.open_until_ms === null || probe !== null)) return null;
  if (value.phase === "half_open" && (value.open_until_ms === null || probe === null)) return null;
  if (probe && probe.generation !== value.generation) return null;
  return {
    v: 1,
    phase: value.phase,
    failure_at_ms: [...value.failure_at_ms].sort((left, right) => left - right).slice(-OPENROUTER_FAILURE_THRESHOLD),
    open_until_ms: value.open_until_ms,
    generation: value.generation,
    probe,
    updated_at_ms: value.updated_at_ms,
  };
};

const recentFailures = (state: OpenRouterCircuitState, nowMs: number): number[] =>
  state.failure_at_ms
    .filter((time) => time >= nowMs - OPENROUTER_FAILURE_WINDOW_MS && time <= nowMs)
    .slice(-OPENROUTER_FAILURE_THRESHOLD);

const readCircuit = (kv: Deno.Kv): Promise<Deno.KvEntryMaybe<OpenRouterCircuitState>> =>
  kv.get<OpenRouterCircuitState>(OPENROUTER_CIRCUIT_KEY, { consistency: "strong" });

const setCircuit = (
  kv: Deno.Kv,
  entry: Deno.KvEntryMaybe<OpenRouterCircuitState>,
  next: OpenRouterCircuitState,
): Promise<Deno.KvCommitResult | Deno.KvCommitError> =>
  kv.atomic()
    .check(entry)
    .set(OPENROUTER_CIRCUIT_KEY, next, { expireIn: OPENROUTER_CIRCUIT_TTL_MS })
    .commit();

const probeMatches = (state: OpenRouterCircuitState, probe: OpenRouterCircuitProbe): boolean =>
  state.phase === "half_open" && state.probe?.token === probe.token &&
  state.probe.generation === probe.generation && state.generation === probe.generation;

const claimProbe = async (
  kv: Deno.Kv,
  source: OpenRouterCircuitProbe["source"],
  nowMs: number,
  newToken: () => string,
): Promise<OpenRouterCircuitProbe | null> => {
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const entry = await readCircuit(kv);
    const state = parseOpenRouterCircuitState(entry.value);
    if (!state || state.phase === "closed" || state.open_until_ms === null) return null;
    if (state.phase === "half_open" && state.probe && nowMs < state.probe.lease_until_ms) return null;
    const generation = state.generation + 1;
    const probe: OpenRouterCircuitProbe = {
      token: newToken(),
      generation,
      lease_until_ms: nowMs + OPENROUTER_PROBE_LEASE_MS,
      source,
    };
    const next: OpenRouterCircuitState = {
      ...state,
      phase: "half_open",
      generation,
      probe,
      updated_at_ms: nowMs,
    };
    if ((await setCircuit(kv, entry, next)).ok) return probe;
  }
  return null;
};

const unavailableDecision = (): OpenRouterCircuitDecision => ({
  route: "codex",
  reason: "unavailable",
  probe: null,
  transition: "none",
});

export const selectOpenRouterCircuitRoute = async (
  nowMs = Date.now(),
  newToken: () => string = () => crypto.randomUUID(),
): Promise<OpenRouterCircuitDecision> => {
  const kv = await getKv();
  if (!kv) return unavailableDecision();
  try {
    const initial = parseOpenRouterCircuitState((await readCircuit(kv)).value);
    if (!initial || initial.phase === "closed" || initial.open_until_ms === null) {
      return { route: "codex", reason: "closed", probe: null, transition: "none" };
    }
    if (initial.phase === "open" && nowMs < initial.open_until_ms) {
      return { route: "openrouter", reason: "open", probe: null, transition: "none" };
    }
    if (initial.phase === "half_open" && initial.probe && nowMs < initial.probe.lease_until_ms) {
      return { route: "openrouter", reason: "concurrent_probe", probe: null, transition: "none" };
    }
    const probe = await claimProbe(kv, "expiry", nowMs, newToken);
    return probe
      ? { route: "codex", reason: "probe", probe, transition: "probe_claimed" }
      : { route: "openrouter", reason: "concurrent_probe", probe: null, transition: "none" };
  } catch {
    return unavailableDecision();
  }
};

export const recordOpenRouterEligibleFailure = async (
  probe: OpenRouterCircuitProbe | null = null,
  nowMs = Date.now(),
): Promise<OpenRouterCircuitTransition> => {
  const kv = await getKv();
  if (!kv) return "none";
  try {
    for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const entry = await readCircuit(kv);
      const state = parseOpenRouterCircuitState(entry.value) ?? closedState(nowMs);
      if (probe && !probeMatches(state, probe)) return "none";
      if (!probe && state.phase === "half_open" && state.probe && nowMs < state.probe.lease_until_ms) {
        // An older ordinary request may fail after another request claims the
        // recovery probe. Only that probe owner may close or reopen its lease.
        return "none";
      }
      if (probe) {
        const next: OpenRouterCircuitState = {
          ...state,
          phase: "open",
          open_until_ms: nowMs + OPENROUTER_OPEN_MS,
          generation: state.generation + 1,
          probe: null,
          updated_at_ms: nowMs,
        };
        if ((await setCircuit(kv, entry, next)).ok) return "reopened";
        continue;
      }
      const failures = [...recentFailures(state, nowMs), nowMs].slice(-OPENROUTER_FAILURE_THRESHOLD);
      const shouldOpen = state.phase !== "closed" || failures.length >= OPENROUTER_FAILURE_THRESHOLD;
      const next: OpenRouterCircuitState = {
        ...state,
        phase: shouldOpen ? "open" : "closed",
        failure_at_ms: failures,
        open_until_ms: shouldOpen ? nowMs + OPENROUTER_OPEN_MS : null,
        generation: shouldOpen && state.phase === "closed" ? state.generation + 1 : state.generation,
        probe: null,
        updated_at_ms: nowMs,
      };
      if ((await setCircuit(kv, entry, next)).ok) {
        return shouldOpen ? (state.phase === "closed" ? "opened" : "reopened") : "none";
      }
    }
  } catch {
    // Circuit persistence must never block the normal provider route.
  }
  return "none";
};

export const closeOpenRouterCircuit = async (
  probe: OpenRouterCircuitProbe,
  nowMs = Date.now(),
): Promise<OpenRouterCircuitTransition> => {
  const kv = await getKv();
  if (!kv) return "none";
  try {
    for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const entry = await readCircuit(kv);
      const state = parseOpenRouterCircuitState(entry.value);
      if (!state || !probeMatches(state, probe)) return "none";
      if ((await setCircuit(kv, entry, closedState(nowMs, state.generation + 1))).ok) return "closed";
    }
  } catch {
    // Best-effort routing state only.
  }
  return "none";
};

export const releaseOpenRouterCircuitProbe = async (
  probe: OpenRouterCircuitProbe,
  nowMs = Date.now(),
): Promise<OpenRouterCircuitTransition> => {
  const kv = await getKv();
  if (!kv) return "none";
  try {
    for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const entry = await readCircuit(kv);
      const state = parseOpenRouterCircuitState(entry.value);
      if (!state || !probeMatches(state, probe)) return "none";
      const next: OpenRouterCircuitState = {
        ...state,
        phase: "open",
        // The probe already waited through the open interval. Releasing a
        // cancelled lease must only remove that claimant; keeping the expiry
        // at the release instant lets the next real request atomically claim
        // the replacement half-open probe instead of imposing another full
        // outage window.
        open_until_ms: nowMs,
        generation: state.generation + 1,
        probe: null,
        updated_at_ms: nowMs,
      };
      if ((await setCircuit(kv, entry, next)).ok) return "released";
    }
  } catch {
    // Best-effort routing state only.
  }
  return "none";
};

export const claimOpenRouterEarlyRecoveryProbe = async (
  nowMs = Date.now(),
  newToken: () => string = () => crypto.randomUUID(),
): Promise<OpenRouterCircuitProbe | null> => {
  const kv = await getKv();
  if (!kv) return null;
  try {
    return await claimProbe(kv, "early_recovery", nowMs, newToken);
  } catch {
    return null;
  }
};

export const getOpenRouterCircuitView = async (nowMs = Date.now()): Promise<Record<string, unknown>> => {
  const kv = await getKv();
  if (!kv) {
    return { available: false, state: "unknown", open_until_ms: null, recent_failures: null, probe_active: null };
  }
  try {
    const state = parseOpenRouterCircuitState((await readCircuit(kv)).value);
    if (!state) {
      return { available: true, state: "closed", open_until_ms: null, recent_failures: 0, probe_active: false };
    }
    const probeActive = state.phase === "half_open" && state.probe !== null && nowMs < state.probe.lease_until_ms;
    return {
      available: true,
      state: state.phase === "closed" ? "closed" : probeActive ? "half_open" : "open",
      open_until_ms: state.open_until_ms,
      recent_failures: recentFailures(state, nowMs).length,
      probe_active: probeActive,
    };
  } catch {
    return { available: false, state: "unknown", open_until_ms: null, recent_failures: null, probe_active: null };
  }
};
