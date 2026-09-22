import { isRecord } from "./utils.ts";

export const CODEX_RESET_USAGE_PREFIX = ["uos_ai", "codex_reset_usage"] as const;

/**
 * The retired gateway-wide switch. The one-way migration and the fail-closed
 * pending check below read it; a completed migration marker stops both, so it
 * never becomes a permanent parent gate.
 */
export const LEGACY_CODEX_BANKED_RESET_USAGE_KEY = ["uos_ai", "codex_banked_reset_usage", "v1"] as const;

/**
 * Completion marker for the one-way migration. It deliberately lives under the
 * existing `uos_ai/migrations` prefix because every `codex_reset_usage` row is
 * validated as an account record by the KV migration audit.
 */
export const CODEX_BANKED_RESET_USAGE_MIGRATION_KEY = ["uos_ai", "migrations", "codex_banked_reset_usage_v1"] as const;

// Stable account identity, independent of API keys and subscription ordering.
export const codexResetUsageKey = (accountIdHash: string): Deno.KvKey => [...CODEX_RESET_USAGE_PREFIX, "account", "v1", accountIdHash];

/**
 * True while the retired gateway-wide opt-out has not been converted. Reading
 * the completion marker first means the legacy key is consulted only until the
 * one-way migration finishes; afterwards every read short-circuits here.
 */
const legacyGatewayResetOptOutPending = async (kv: Deno.Kv): Promise<boolean> => {
  const marker = await kv.get(CODEX_BANKED_RESET_USAGE_MIGRATION_KEY, { consistency: "strong" });
  if (marker.value !== null) return false;
  const legacy = await kv.get<unknown>(LEGACY_CODEX_BANKED_RESET_USAGE_KEY, { consistency: "strong" });
  return legacy.value !== null && legacy.value !== true;
};

export const readCodexResetUsage = async (kv: Deno.Kv, accountIdHash: string) => {
  const entry = await kv.get(codexResetUsageKey(accountIdHash), { consistency: "strong" });
  if (entry.value !== null) {
    return { allowed: isRecord(entry.value) && entry.value.enabled === true, entries: [entry] };
  }
  // A missing subscription row means enabled, but every subscription fails
  // closed while a legacy opt-out is still waiting for its one-way migration.
  if (await legacyGatewayResetOptOutPending(kv)) return { allowed: false, entries: [entry] };
  return { allowed: true, entries: [entry] };
};

export type CodexBankedResetUsageMigration =
  | Readonly<{ kind: "already_migrated" }>
  | Readonly<{ kind: "no_legacy_setting" }>
  | Readonly<{ kind: "migrated"; legacyEnabled: boolean; disabledAccounts: number }>;

/**
 * One-way migration from the removed gateway-wide banked-reset switch to the
 * per-subscription records the serving path reads.
 *
 * The retired switch allowed resets only for an explicit `true`; a `false` or
 * malformed value kept the operator's opt-out. Because the serving path now
 * treats a missing subscription row as enabled, that opt-out would otherwise
 * disappear on upgrade. Every account that exists at migration time is given
 * `{ enabled: false }` unless it already has an operator-written row, and the
 * completion marker then retires both the legacy key and the fail-closed
 * pending check. Subscriptions added after the migration are therefore not
 * gated by the removed setting, and a completed migration is never re-read or
 * re-applied.
 */
export const migrateLegacyCodexBankedResetUsage = async (
  kv: Deno.Kv,
  accountIdHashes: readonly string[],
  nowMs = Date.now()
): Promise<CodexBankedResetUsageMigration> => {
  const marker = await kv.get(CODEX_BANKED_RESET_USAGE_MIGRATION_KEY, { consistency: "strong" });
  if (marker.value !== null) return { kind: "already_migrated" };
  const legacy = await kv.get<unknown>(LEGACY_CODEX_BANKED_RESET_USAGE_KEY, { consistency: "strong" });
  if (legacy.value === null) return { kind: "no_legacy_setting" };
  const legacyEnabled = legacy.value === true;
  let disabledAccounts = 0;
  if (!legacyEnabled) {
    for (const accountIdHash of new Set(accountIdHashes)) {
      if (!accountIdHash) continue;
      const key = codexResetUsageKey(accountIdHash);
      const existing = await kv.get(key, { consistency: "strong" });
      // An explicit per-subscription choice written by the operator wins.
      if (existing.value !== null) continue;
      const commit = await kv.atomic().check(existing).set(key, { enabled: false }).commit();
      if (commit.ok) disabledAccounts += 1;
    }
  }
  const markerRecord = { version: 1, legacy_enabled: legacyEnabled, disabled_accounts: disabledAccounts, completed_at_ms: nowMs };
  const completed = await kv.atomic().check(marker).set(CODEX_BANKED_RESET_USAGE_MIGRATION_KEY, markerRecord).commit();
  // A concurrent start may have completed the same migration first; both runs
  // wrote identical per-subscription rows, so the marker alone decides.
  if (!completed.ok) return { kind: "already_migrated" };
  return { kind: "migrated", legacyEnabled, disabledAccounts };
};
