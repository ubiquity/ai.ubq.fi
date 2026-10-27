import type { Kv } from "#kv";
import { currentClaudeCodeAccountEntry, type ClaudeCodeAccountBinding } from "./claude-code-accounts.ts";
import { loadClaudeCodeQuotaState, recordClaudeCodeQuotaOutcome, type ClaudeCodeQuotaState, type ClaudeCodeUsageState } from "./claude-code-quota.ts";
import { fetchClaudeCodeUsage, type ClaudeCodeUsageResult } from "./claude-code-usage.ts";
import type { ClaudeCodeFetch } from "./claude-code.ts";

/** Matches the existing visible Analytics capacity poll. Inference headers do not renew this clock. */
export const CLAUDE_CODE_USAGE_FRESH_MS = 30_000;
const USAGE_LEASE_MS = 10_000;
type UsageLease = Readonly<{ owner: string; until_ms: number }>;
export type ClaudeCodeUsageRefresh =
  | Readonly<{ kind: "changed" }>
  | Readonly<{ kind: "busy" }>
  | Readonly<{ kind: "fresh"; usage: ClaudeCodeUsageState }>
  | Readonly<{ kind: "completed"; result: ClaudeCodeUsageResult; usage: ClaudeCodeUsageState }>;

export const claudeCodeUsageIsFresh = (quota: ClaudeCodeQuotaState | null, nowMs: number): boolean =>
  quota?.usage !== null &&
  quota?.usage !== undefined &&
  nowMs >= quota.usage.attempted_at_ms &&
  nowMs - quota.usage.attempted_at_ms < CLAUDE_CODE_USAGE_FRESH_MS;

/** Shared by automatic capacity capture and the targeted manual action. No credential mutation occurs here. */
export const refreshClaudeCodeUsage = async (
  kv: Kv,
  binding: ClaudeCodeAccountBinding,
  options: Readonly<{ fetcher?: ClaudeCodeFetch; signal?: AbortSignal; now?: () => number }> = {}
): Promise<ClaudeCodeUsageRefresh> => {
  const clock = options.now ?? Date.now;
  const nowMs = Math.trunc(clock());
  const account = await currentClaudeCodeAccountEntry(kv, binding);
  if (!account) return { kind: "changed" };
  const quota = await loadClaudeCodeQuotaState(kv, binding);
  if (quota?.usage && claudeCodeUsageIsFresh(quota, nowMs)) return { kind: "fresh", usage: quota.usage };
  const key = ["uos_ai", "claude_code_usage", "v1", "lease", binding.account_id, binding.account_cohort_id] as const;
  const entry = await kv.get<UsageLease>(key, { consistency: "strong" });
  if (entry.value && entry.value.until_ms > nowMs) return { kind: "busy" };
  const owner = crypto.randomUUID();
  const claimed = await kv
    .atomic()
    .check(account, entry)
    .set(key, { owner, until_ms: nowMs + USAGE_LEASE_MS }, { expireIn: USAGE_LEASE_MS * 2 })
    .commit();
  if (!claimed.ok) return { kind: "busy" };
  try {
    // A refresh may have committed between our first quota read and lease acquisition.
    const latest = await loadClaudeCodeQuotaState(kv, binding);
    if (latest?.usage && claudeCodeUsageIsFresh(latest, Math.trunc(clock()))) return { kind: "fresh", usage: latest.usage };
    if (!(await currentClaudeCodeAccountEntry(kv, binding))) return { kind: "changed" };
    const result = await fetchClaudeCodeUsage(binding.credential, options);
    const usage: ClaudeCodeUsageState = {
      source: "oauth_usage",
      state: result.ok ? "ok" : result.kind,
      status: result.status,
      message: result.ok ? null : result.message,
      attempted_at_ms: result.observed_at_ms,
      observed_at_ms: result.ok ? result.observed_at_ms : null,
    };
    const recorded = await recordClaudeCodeQuotaOutcome(kv, {
      binding,
      observation: result.ok ? result.observation : null,
      response_status: result.status,
      response_observed_at_ms: result.observed_at_ms,
      usage,
      nowMs: result.observed_at_ms,
    });
    return recorded ? { kind: "completed", result, usage } : { kind: "changed" };
  } finally {
    const current = await kv.get<UsageLease>(key, { consistency: "strong" });
    if (current.value?.owner === owner) await kv.atomic().check(current).delete(key).commit();
  }
};
