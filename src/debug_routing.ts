import { getKv } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";

export const DEBUG_ROUTING_KEY = ["uos_ai", "debug_routing", "v1"] as const;
export const DEBUG_ROUTING_MAX_DURATION_MS = 60 * 60_000;

export type DebugRoutingScenario =
  | "normal"
  | "metered_first"
  | "openrouter_first"
  | "codex_401"
  | "codex_403"
  | "codex_429";

export type DebugRoutingConfig = Readonly<{
  scenario: DebugRoutingScenario;
  expires_at_ms: number | null;
  updated_at_ms: number;
}>;

const SCENARIOS = new Set<DebugRoutingScenario>([
  "normal",
  "metered_first",
  "openrouter_first",
  "codex_401",
  "codex_403",
  "codex_429",
]);

const normalize = (value: unknown): DebugRoutingConfig | null => {
  if (!isRecord(value)) return null;
  const scenario = getString(value.scenario)?.trim() as DebugRoutingScenario | undefined;
  if (!scenario || !SCENARIOS.has(scenario)) return null;
  const expires = value.expires_at_ms;
  const expiresAtMs = expires === null
    ? null
    : typeof expires === "number" && Number.isSafeInteger(expires) && expires > 0
    ? expires
    : null;
  const updated = value.updated_at_ms;
  const updatedAtMs = typeof updated === "number" && Number.isSafeInteger(updated) && updated > 0 ? updated : 0;
  if (!updatedAtMs) return null;
  return { scenario, expires_at_ms: expiresAtMs, updated_at_ms: updatedAtMs };
};

let cached: Readonly<{ value: DebugRoutingConfig; expiresAtMs: number }> | null = null;
const CACHE_TTL_MS = 5_000;

export const loadDebugRoutingConfig = async (nowMs = Date.now()): Promise<DebugRoutingConfig> => {
  if (cached && cached.expiresAtMs > nowMs) {
    if (cached.value.expires_at_ms !== null && cached.value.expires_at_ms <= nowMs) return normalConfig(nowMs);
    return cached.value;
  }
  try {
    const kv = await getKv();
    if (!kv) return normalConfig(nowMs);
    const entry = await kv.get<unknown>(DEBUG_ROUTING_KEY, { consistency: "strong" });
    const value = normalize(entry.value);
    if (!value || (value.expires_at_ms !== null && value.expires_at_ms <= nowMs)) return normalConfig(nowMs);
    cached = { value, expiresAtMs: nowMs + CACHE_TTL_MS };
    return value;
  } catch {
    return normalConfig(nowMs);
  }
};

const normalConfig = (nowMs: number): DebugRoutingConfig => ({
  scenario: "normal",
  expires_at_ms: null,
  updated_at_ms: nowMs,
});

export const setDebugRoutingConfig = async (
  scenario: DebugRoutingScenario,
  durationMs: number,
  nowMs = Date.now(),
): Promise<DebugRoutingConfig> => {
  if (!SCENARIOS.has(scenario)) throw new Error("Unknown debug routing scenario");
  if (!Number.isSafeInteger(durationMs) || durationMs < 0 || durationMs > DEBUG_ROUTING_MAX_DURATION_MS) {
    throw new Error("Debug routing duration must be between 0 and 3600000 milliseconds");
  }
  const value: DebugRoutingConfig = {
    scenario,
    expires_at_ms: scenario === "normal" || durationMs === 0 ? null : nowMs + durationMs,
    updated_at_ms: nowMs,
  };
  const kv = await getKv();
  if (!kv) throw new Error("Deno KV is unavailable");
  await kv.set(DEBUG_ROUTING_KEY, value, { expireIn: DEBUG_ROUTING_MAX_DURATION_MS + 5 * 60_000 });
  cached = { value, expiresAtMs: nowMs + CACHE_TTL_MS };
  return value;
};

export const resetDebugRoutingCacheForTest = (): void => {
  cached = null;
};
