/**
 * Shared in-process protection for the two best-effort prompt-cache writers.
 * The scope is the supplied KV object when one is explicit; otherwise all
 * default/unavailable KV lookups share one local scope.
 */
export const PROMPT_CACHE_KV_COOLDOWN_BASE_MS = 5_000;
export const PROMPT_CACHE_KV_COOLDOWN_MAX_MS = 60_000;
export const PROMPT_CACHE_KV_COOLDOWN_MIN_MS = 4_000;
export const PROMPT_CACHE_KV_COOLDOWN_PROBE_WINDOW_MS = 5_000;
export const PROMPT_CACHE_KV_COOLDOWN_JITTER = 0.2;

type PromptCacheKvCooldownState = {
  blockedUntilMs: number;
  probeExpiresAtMs: number;
  probeToken: number | null;
  nextProbeToken: number;
  consecutiveFailures: number;
};

export type PromptCacheKvCooldownOptions = Readonly<{
  now?: () => number;
  random?: () => number;
}>;

export type PromptCacheKvCooldownLease = Readonly<{
  admitted: boolean;
  probe: boolean;
  succeed: () => void;
  fail: () => void;
  release: () => void;
}>;

const SHARED_SCOPE = {};
let states = new WeakMap<object, PromptCacheKvCooldownState>();

const newState = (): PromptCacheKvCooldownState => ({
  blockedUntilMs: 0,
  probeExpiresAtMs: 0,
  probeToken: null,
  nextProbeToken: 0,
  consecutiveFailures: 0,
});

const stateFor = (scope: object | null | undefined): PromptCacheKvCooldownState => {
  const key = scope ?? SHARED_SCOPE;
  let state = states.get(key);
  if (!state) {
    state = newState();
    states.set(key, state);
  }
  return state;
};

const nowFor = (now: (() => number) | undefined): number => {
  try {
    const value = now?.() ?? Date.now();
    return Number.isFinite(value) && value >= 0 ? value : Date.now();
  } catch {
    return Date.now();
  }
};

const randomFor = (random: (() => number) | undefined): number => {
  try {
    const value = random?.() ?? Math.random();
    return Number.isFinite(value) && value >= 0 && value <= 1 ? value : 0.5;
  } catch {
    return 0.5;
  }
};

const cooldownDurationMs = (
  state: PromptCacheKvCooldownState,
  random: (() => number) | undefined,
): number => {
  const backoff = Math.min(
    PROMPT_CACHE_KV_COOLDOWN_MAX_MS,
    PROMPT_CACHE_KV_COOLDOWN_BASE_MS * 2 ** Math.min(state.consecutiveFailures - 1, 4),
  );
  const jitter = (randomFor(random) * 2 - 1) * PROMPT_CACHE_KV_COOLDOWN_JITTER;
  return Math.min(
    PROMPT_CACHE_KV_COOLDOWN_MAX_MS,
    Math.max(PROMPT_CACHE_KV_COOLDOWN_MIN_MS, Math.round(backoff * (1 + jitter))),
  );
};

const deniedLease = (): PromptCacheKvCooldownLease => ({
  admitted: false,
  probe: false,
  succeed: () => {},
  fail: () => {},
  release: () => {},
});

/**
 * Admits normal writes while healthy, one probe after a cooldown, and no
 * writes while the probe is in flight. Completion is explicitly reported by
 * the caller so a successful probe is the only event that closes the breaker.
 */
export const acquirePromptCacheKvCooldown = (
  scope: object | null | undefined,
  options: PromptCacheKvCooldownOptions = {},
): PromptCacheKvCooldownLease => {
  const state = stateFor(scope);
  const now = nowFor(options.now);
  if (state.probeToken !== null && now >= state.probeExpiresAtMs) {
    state.probeToken = null;
    state.probeExpiresAtMs = 0;
  }
  if (state.blockedUntilMs > now || state.probeToken !== null) return deniedLease();

  const isProbe = state.consecutiveFailures > 0;
  const probeToken = isProbe ? ++state.nextProbeToken : null;
  if (isProbe) {
    state.probeToken = probeToken;
    state.probeExpiresAtMs = now + PROMPT_CACHE_KV_COOLDOWN_PROBE_WINDOW_MS;
  }

  let finished = false;
  const succeed = (): void => {
    if (finished) return;
    finished = true;
    if (probeToken !== null && state.probeToken === probeToken) {
      state.probeToken = null;
      state.probeExpiresAtMs = 0;
      state.blockedUntilMs = 0;
      state.consecutiveFailures = 0;
    }
  };
  const fail = (): void => {
    if (finished) return;
    finished = true;
    state.probeToken = null;
    state.probeExpiresAtMs = 0;
    state.consecutiveFailures = Math.min(state.consecutiveFailures + 1, 5);
    const failureNow = nowFor(options.now);
    state.blockedUntilMs = Math.max(failureNow, state.blockedUntilMs) + cooldownDurationMs(state, options.random);
  };
  const release = (): void => {
    if (finished) return;
    finished = true;
    if (probeToken !== null && state.probeToken === probeToken) {
      state.probeToken = null;
      state.probeExpiresAtMs = 0;
    }
  };

  return { admitted: true, probe: isProbe, succeed, fail, release };
};

/** Test-only reset; production callers never need to clear process state. */
export const resetPromptCacheKvCooldownForTest = (): void => {
  states = new WeakMap<object, PromptCacheKvCooldownState>();
};
