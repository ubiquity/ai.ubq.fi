import { isRecord } from "./utils.ts";

export const CODEX_RESET_USAGE_PREFIX = ["uos_ai", "codex_reset_usage"] as const;

// Stable account identity, independent of API keys and subscription ordering.
export const codexResetUsageKey = (accountIdHash: string): Deno.KvKey => [...CODEX_RESET_USAGE_PREFIX, "account", "v1", accountIdHash];

export const readCodexResetUsage = async (kv: Deno.Kv, accountIdHash: string) => {
  const entry = await kv.get(codexResetUsageKey(accountIdHash), { consistency: "strong" });
  return {
    allowed: entry.value === null || (isRecord(entry.value) && entry.value.enabled === true),
    entries: [entry],
  };
};
