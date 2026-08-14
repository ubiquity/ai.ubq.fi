import { getKv } from "./kv.ts";

export const DEBUG_TOOLS_KEY = ["uos_ai", "debug_tools", "v1"] as const;

export type DebugToolsState = Readonly<{
  force_codex_primary_503: boolean;
  updated_at_ms: number;
}>;

const DEBUG_TOOLS_CACHE_TTL_MS = 1_000;
let cachedDebugToolsState: DebugToolsState | null = null;
let cachedDebugToolsExpiresAtMs = 0;

export const normalizeDebugToolsState = (value: unknown): DebugToolsState | null => {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const state = value as Record<string, unknown>;
  if (typeof state.force_codex_primary_503 !== "boolean") return null;
  if (
    typeof state.updated_at_ms !== "number" ||
    !Number.isSafeInteger(state.updated_at_ms) ||
    state.updated_at_ms < 0
  ) {
    return null;
  }
  return {
    force_codex_primary_503: state.force_codex_primary_503,
    updated_at_ms: state.updated_at_ms,
  };
};

export const defaultDebugToolsState = (nowMs = Date.now()): DebugToolsState => ({
  force_codex_primary_503: false,
  updated_at_ms: nowMs,
});

export const loadDebugToolsState = async (kv: Deno.Kv): Promise<DebugToolsState> => {
  const entry = await kv.get<unknown>(DEBUG_TOOLS_KEY, { consistency: "strong" });
  return normalizeDebugToolsState(entry.value) ?? defaultDebugToolsState();
};

export const cacheDebugToolsState = (state: DebugToolsState): void => {
  cachedDebugToolsState = state;
  cachedDebugToolsExpiresAtMs = Date.now() + DEBUG_TOOLS_CACHE_TTL_MS;
};

export const resetDebugToolsStateCacheForTest = (): void => {
  cachedDebugToolsState = null;
  cachedDebugToolsExpiresAtMs = 0;
};

export const isCodexPrimary503Forced = async (): Promise<boolean> => {
  if (cachedDebugToolsState && Date.now() < cachedDebugToolsExpiresAtMs) {
    return cachedDebugToolsState.force_codex_primary_503;
  }
  const kv = await getKv();
  if (!kv) return false;
  const state = await loadDebugToolsState(kv);
  cacheDebugToolsState(state);
  return state.force_codex_primary_503;
};
