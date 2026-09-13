import { isRecord } from "./utils.ts";

// Stable account identity, independent of API keys and subscription ordering.
export const codexResetUsageKey = (accountIdHash: string): Deno.KvKey => ["uos_ai", "codex_reset_usage", "account", "v1", accountIdHash];

export const readCodexResetUsage = async (kv: Deno.Kv, accountIdHash: string) => {
  const entry = await kv.get<unknown>(codexResetUsageKey(accountIdHash), { consistency: "strong" });
  return {
    allowed: entry.value === null || (isRecord(entry.value) && entry.value.enabled === true),
    entries: [entry],
  };
};
