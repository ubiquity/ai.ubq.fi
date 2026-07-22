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

type InternalCodexRateLimitDecision = Readonly<{
  decision: CodexRateLimitDecision;
  state: CodexRateLimitState | null;
  versionstamp: string | null | undefined;
}>;

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

const rateLimitEntry = (
  state: CodexRateLimitState,
  versionstamp: string,
): Deno.KvEntry<CodexRateLimitState> => ({
  key: CODEX_RATE_LIMIT_KV_KEY,
  value: state,
  versionstamp,
});

const claimCodexRateLimitProbe = async (
  kv: Deno.Kv,
  entry: Deno.KvEntry<CodexRateLimitState>,
  state: CodexRateLimitState,
  nowMs: number,
  probeId: string,
): Promise<Readonly<{ state: CodexRateLimitState; versionstamp: string }> | null> => {
  const next: CodexRateLimitState = {
    observed_at_ms: state.observed_at_ms,
    retry_at_ms: state.retry_at_ms,
    probe_id: probeId,
    probe_lease_until_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS,
  };
  const result = await kv.atomic().check(entry).set(CODEX_RATE_LIMIT_KV_KEY, next).commit();
  return result.ok ? { state: next, versionstamp: result.versionstamp } : null;
};

const decideCodexRateLimitFromKv = async (
  kv: Deno.Kv | null,
  nowMs: number,
  createProbeId: () => string,
): Promise<InternalCodexRateLimitDecision> => {
  if (!kv) return { decision: { kind: "primary" }, state: null, versionstamp: undefined };
  try {
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
      const state = parseState(entry.value);
      if (!state) {
        return { decision: { kind: "primary" }, state: null, versionstamp: entry.versionstamp };
      }
      if (state.retry_at_ms > nowMs) {
        return {
          decision: { kind: "cached", retryAtMs: state.retry_at_ms },
          state,
          versionstamp: entry.versionstamp,
        };
      }
      if (state.probe_id && state.probe_lease_until_ms! > nowMs) {
        return {
          decision: { kind: "cached", retryAtMs: state.probe_lease_until_ms! },
          state,
          versionstamp: entry.versionstamp,
        };
      }
      if (!entry.versionstamp) continue;
      const probeId = createProbeId();
      const claimed = await claimCodexRateLimitProbe(
        kv,
        rateLimitEntry(state, entry.versionstamp),
        state,
        nowMs,
        probeId,
      );
      if (claimed) {
        return {
          decision: { kind: "probe", probeId },
          state: claimed.state,
          versionstamp: claimed.versionstamp,
        };
      }
    }
    return {
      decision: { kind: "cached", retryAtMs: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS },
      state: { observed_at_ms: nowMs, retry_at_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS },
      versionstamp: undefined,
    };
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit circuit read failed open:", error);
    return { decision: { kind: "primary" }, state: null, versionstamp: undefined };
  }
};

export const getCodexRateLimitDecision = async (
  kv: Deno.Kv | null,
  nowMs = Date.now(),
  createProbeId: () => string = () => crypto.randomUUID(),
): Promise<CodexRateLimitDecision> => {
  return (await decideCodexRateLimitFromKv(kv, nowMs, createProbeId)).decision;
};

let localState: CodexRateLimitState | null = null;
// `undefined` means local state was synthesized or changed without retaining its KV version.
let localVersionstamp: string | null | undefined;
let hydrated = false;
let hydrationInFlight: Promise<void> | null = null;

const hydrateGlobalCodexRateLimitState = async (): Promise<void> => {
  if (hydrated) return;
  if (!hydrationInFlight) {
    hydrationInFlight = (async () => {
      const kv = await kvPromise;
      if (!kv) {
        localState = null;
        localVersionstamp = undefined;
        hydrated = true;
        return;
      }
      try {
        const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
        localState = parseState(entry.value);
        localVersionstamp = entry.versionstamp;
      } catch (error) {
        console.warn("[ai.ubq.fi] Codex rate-limit circuit hydration failed open:", error);
        localState = null;
        localVersionstamp = undefined;
      }
      hydrated = true;
    })().finally(() => {
      hydrationInFlight = null;
    });
  }
  await hydrationInFlight;
};

export const getGlobalCodexRateLimitDecision = async (nowMs = Date.now()): Promise<CodexRateLimitDecision> => {
  await hydrateGlobalCodexRateLimitState();
  // Deliberately avoid re-reading a healthy/null circuit: the warm success path has a zero-read budget.
  // If another isolate opened the shared circuit, concurrent requests here can leak a small burst of primary 429s;
  // each response opens local state, so later requests in this isolate use the cooldown without a healthy-path read.
  if (!localState) return { kind: "primary" };
  if (localState.retry_at_ms > nowMs) return { kind: "cached", retryAtMs: localState.retry_at_ms };
  if (localState.probe_id && localState.probe_lease_until_ms! > nowMs) {
    return { kind: "cached", retryAtMs: localState.probe_lease_until_ms! };
  }

  const kv = await kvPromise;
  if (!kv) {
    localState = null;
    localVersionstamp = undefined;
    return { kind: "primary" };
  }

  // A cold isolate already paid for this entry during hydration. Claim directly against that
  // version so recovery adds no second read, and mark the claim locally before awaiting the CAS
  // so concurrent requests in this isolate cannot all become probes.
  if (typeof localVersionstamp === "string") {
    const previousState = localState;
    const previousVersionstamp = localVersionstamp;
    const probeId = crypto.randomUUID();
    const pendingProbe: CodexRateLimitState = {
      observed_at_ms: previousState.observed_at_ms,
      retry_at_ms: previousState.retry_at_ms,
      probe_id: probeId,
      probe_lease_until_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS,
    };
    localState = pendingProbe;
    localVersionstamp = undefined;
    try {
      const claimed = await claimCodexRateLimitProbe(
        kv,
        rateLimitEntry(previousState, previousVersionstamp),
        previousState,
        nowMs,
        probeId,
      );
      if (claimed) {
        if (localState?.probe_id === probeId) {
          localState = claimed.state;
          localVersionstamp = claimed.versionstamp;
        }
        return { kind: "probe", probeId };
      }
      if (localState?.probe_id === probeId) {
        localState = { observed_at_ms: nowMs, retry_at_ms: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS };
        localVersionstamp = undefined;
      }
      return { kind: "cached", retryAtMs: nowMs + CODEX_RATE_LIMIT_PROBE_LEASE_MS };
    } catch (error) {
      console.warn("[ai.ubq.fi] Codex rate-limit circuit probe failed open:", error);
      if (localState?.probe_id === probeId) {
        localState = null;
        localVersionstamp = undefined;
      }
      return { kind: "primary" };
    }
  }

  // Local 429 updates intentionally retain no versionstamp. At probe expiry, refresh once to
  // coordinate globally; this remains off the healthy request path.
  const resolved = await decideCodexRateLimitFromKv(kv, nowMs, () => crypto.randomUUID());
  localState = resolved.state;
  localVersionstamp = resolved.versionstamp;
  return resolved.decision;
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
  localVersionstamp = undefined;
  hydrated = true;
  return retryAtMs;
};

const deleteCodexRateLimitProbeEntry = async (
  kv: Deno.Kv,
  entry: Deno.KvEntry<CodexRateLimitState>,
): Promise<boolean> => (await kv.atomic().check(entry).delete(CODEX_RATE_LIMIT_KV_KEY).commit()).ok;

export const closeCodexRateLimitProbe = async (kv: Deno.Kv | null, probeId: string): Promise<void> => {
  if (!kv) return;
  try {
    const entry = await kv.get<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY, { consistency: "strong" });
    const state = parseState(entry.value);
    if (!state || !entry.versionstamp || state.probe_id !== probeId) return;
    await deleteCodexRateLimitProbeEntry(kv, rateLimitEntry(state, entry.versionstamp));
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit probe close failed:", error);
  }
};

export const closeGlobalCodexRateLimitProbe = async (probeId: string): Promise<void> => {
  const state = localState;
  const versionstamp = localVersionstamp;
  if (!state || state.probe_id !== probeId || typeof versionstamp !== "string") return;
  const kv = await kvPromise;
  if (!kv) return;
  try {
    const deleted = await deleteCodexRateLimitProbeEntry(kv, rateLimitEntry(state, versionstamp));
    if (localState?.probe_id !== probeId || localVersionstamp !== versionstamp) return;
    if (deleted) {
      localState = null;
      localVersionstamp = null;
    } else {
      // A newer cross-isolate write won the race. Preserve it by refusing to delete and force
      // a single refresh only after this local lease expires.
      localVersionstamp = undefined;
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Codex rate-limit probe close failed:", error);
  }
};

export const resetCodexRateLimitCacheForTest = (): void => {
  localState = null;
  localVersionstamp = undefined;
  hydrated = false;
  hydrationInFlight = null;
};
