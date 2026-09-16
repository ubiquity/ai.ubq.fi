import assert from "node:assert/strict";
import {
  applyCodexAuthPlan,
  type AssessedCodexAuthCandidate,
  type CodexAuthCandidate,
  codexAuthCandidateFromAuthJson,
  codexAuthCandidateKey,
  codexAuthCandidatesFromPool,
  isCodexAuthCandidateUsable,
  planCodexAuthRepair,
  rankCodexAuthCandidates,
} from "../src/codex_auth_repair.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const NOW_MS = Date.UTC(2026, 8, 16, 18, 0, 0);

/** Drops base64url `=` padding without a backtracking-prone trailing regex. */
const stripBase64Padding = (value: string): string => {
  let end = value.length;
  while (end > 0 && value[end - 1] === "=") end -= 1;
  return value.slice(0, end);
};

/** Builds a three-part token whose payload carries the requested `exp` claim. */
const tokenExpiringAt = (expMs: number | null): string => {
  const encode = (value: unknown): string => {
    const base64 = btoa(JSON.stringify(value)).replaceAll("+", "-").replaceAll("/", "_");
    return stripBase64Padding(base64);
  };
  const payload = expMs === null ? {} : { exp: Math.floor(expMs / 1000) };
  return `header.${encode(payload)}.signature`;
};

const account = (accountId: string, expMs: number | null, refresh = `refresh-${accountId}`): CodexAuthState => ({
  account_id: accountId,
  access_token: tokenExpiringAt(expMs),
  refresh_token: refresh,
  updated_at_ms: NOW_MS - 60_000,
});

const poolOf = (accounts: readonly CodexAuthState[]): CodexAuthPoolState => ({ accounts, updated_at_ms: NOW_MS - 60_000 });

const candidate = (
  source: CodexAuthCandidate["source"],
  accountId: string,
  expMs: number | null,
  options: Readonly<{ refresh?: string; updated_at_ms?: number | null }> = {}
): CodexAuthCandidate => ({
  source,
  account_id: accountId,
  access_token: tokenExpiringAt(expMs),
  refresh_token: options.refresh ?? `refresh-${accountId}`,
  updated_at_ms: options.updated_at_ms === undefined ? NOW_MS : options.updated_at_ms,
});

const assessed = (value: CodexAuthCandidate, probe: AssessedCodexAuthCandidate["probe"]): AssessedCodexAuthCandidate => ({
  candidate: value,
  access_exp_ms: expFromToken(value.access_token),
  probe,
});

const expFromToken = (token: string): number | null => {
  const part = token.split(".")[1] ?? "";
  if (!part) return null;
  try {
    const parsed = JSON.parse(atob(part.replace(/-/g, "+").replace(/_/g, "/"))) as { exp?: unknown };
    return typeof parsed.exp === "number" ? parsed.exp * 1000 : null;
  } catch {
    return null;
  }
};

const valid = (status = 200): AssessedCodexAuthCandidate["probe"] => ({ outcome: "valid", status });
const invalid = (status = 401): AssessedCodexAuthCandidate["probe"] => ({ outcome: "invalid", status });
const inconclusive = (status: number | null = null): AssessedCodexAuthCandidate["probe"] => ({ outcome: "inconclusive", status });

const localAccount = (slot: number, value: CodexAuthState, probe: AssessedCodexAuthCandidate["probe"], refreshDiverged: boolean | null = null) => ({
  slot,
  account: value,
  probe,
  refresh_diverged: refreshDiverged,
});

Deno.test("isCodexAuthCandidateUsable requires an accepted token that has not expired", () => {
  const live = candidate("vps-pool", "acct-a", NOW_MS + 3_600_000);
  assert.equal(isCodexAuthCandidateUsable(assessed(live, valid()), NOW_MS), true);
  assert.equal(isCodexAuthCandidateUsable(assessed(live, valid(429)), NOW_MS), true);
  assert.equal(isCodexAuthCandidateUsable(assessed(live, invalid()), NOW_MS), false);
  assert.equal(isCodexAuthCandidateUsable(assessed(live, inconclusive()), NOW_MS), false);

  // A token the upstream still accepts can be seconds from expiry; the claim is
  // checked on its own so a stale credential is never adopted.
  const expired = candidate("vps-pool", "acct-a", NOW_MS - 1);
  assert.equal(isCodexAuthCandidateUsable(assessed(expired, valid()), NOW_MS), false);
});

Deno.test("rankCodexAuthCandidates prefers the longest-lived credential and drops unusable ones", () => {
  const shortLived = assessed(candidate("vps-pool", "acct-a", NOW_MS + 60_000), valid());
  const longLived = assessed(candidate("local-auth-json", "acct-a", NOW_MS + 86_400_000), valid());
  const rejected = assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 172_800_000), invalid());

  const ranked = rankCodexAuthCandidates([shortLived, longLived, rejected], NOW_MS);
  assert.deepEqual(
    ranked.map((entry) => entry.candidate.source),
    ["local-auth-json", "vps-pool"]
  );
});

Deno.test("rankCodexAuthCandidates breaks expiry ties by freshness and then by source order", () => {
  const expMs = NOW_MS + 3_600_000;
  const stale = candidate("vps-auth-json", "acct-a", expMs, { updated_at_ms: NOW_MS - 90_000 });
  const fresh = candidate("vps-pool", "acct-a", expMs, { updated_at_ms: NOW_MS - 1_000 });
  const untimed = candidate("vps-pool", "acct-b", expMs, { updated_at_ms: null });

  const ranked = rankCodexAuthCandidates([assessed(stale, valid()), assessed(fresh, valid()), assessed(untimed, valid())], NOW_MS);
  assert.deepEqual(
    ranked.map((entry) => entry.candidate.updated_at_ms),
    [NOW_MS - 1_000, NOW_MS - 90_000, null]
  );

  const sameExpirySameFreshness = [
    assessed(candidate("vps-auth-json", "acct-a", expMs), valid()),
    assessed(candidate("local-auth-json", "acct-a", expMs), valid()),
  ];
  assert.deepEqual(
    rankCodexAuthCandidates(sameExpirySameFreshness, NOW_MS).map((entry) => entry.candidate.source),
    ["local-auth-json", "vps-auth-json"]
  );
});

Deno.test("planCodexAuthRepair leaves an authenticating account untouched", () => {
  const local = account("acct-a", NOW_MS + 3_600_000);
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, local, valid())],
    candidates: [assessed(candidate("vps-pool", "acct-a", NOW_MS + 86_400_000), valid())],
    nowMs: NOW_MS,
  });

  assert.equal(plan.selections.length, 0);
  assert.equal(plan.unrepairable.length, 0);
  assert.deepEqual(plan.healthy, [{ slot: 1, account_id: "acct-a", refresh_diverged: null }]);
});

Deno.test("planCodexAuthRepair treats a probe-valid but already expired local token as broken", () => {
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, account("acct-a", NOW_MS - 1_000), valid())],
    candidates: [assessed(candidate("vps-pool", "acct-a", NOW_MS + 3_600_000), valid())],
    nowMs: NOW_MS,
  });

  assert.equal(plan.healthy.length, 0);
  assert.equal(plan.selections.length, 1);
  assert.equal(plan.selections[0].candidate.source, "vps-pool");
});

Deno.test("planCodexAuthRepair repairs a rejected slot from the best candidate for that account", () => {
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, account("acct-a", NOW_MS + 3_600_000), invalid())],
    candidates: [
      assessed(candidate("local-auth-json", "acct-a", NOW_MS + 60_000), valid()),
      assessed(candidate("vps-pool", "acct-a", NOW_MS + 86_400_000), valid()),
      assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 172_800_000), invalid()),
    ],
    nowMs: NOW_MS,
  });

  assert.equal(plan.selections.length, 1);
  assert.equal(plan.selections[0].slot, 1);
  assert.equal(plan.selections[0].account_id, "acct-a");
  assert.equal(plan.selections[0].candidate.source, "vps-pool");
  assert.deepEqual(
    plan.selections[0].rejected.map((entry) => `${entry.source}:${entry.outcome}`),
    ["local-auth-json:valid", "vps-auth-json:invalid"]
  );
});

Deno.test("planCodexAuthRepair never adopts another account's credential", () => {
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, account("acct-a", NOW_MS + 3_600_000), invalid())],
    candidates: [assessed(candidate("vps-pool", "acct-b", NOW_MS + 86_400_000), valid())],
    nowMs: NOW_MS,
  });

  assert.equal(plan.selections.length, 0);
  assert.equal(plan.unrepairable.length, 1);
  assert.equal(plan.unrepairable[0].reason, "no_candidates");
});

Deno.test("planCodexAuthRepair reports a blocked account when no candidate authenticates", () => {
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, account("acct-a", NOW_MS + 3_600_000), invalid())],
    candidates: [
      assessed(candidate("vps-pool", "acct-a", NOW_MS + 86_400_000), invalid()),
      assessed(candidate("vps-auth-json", "acct-a", NOW_MS + 3_600_000), inconclusive()),
    ],
    nowMs: NOW_MS,
  });

  assert.equal(plan.selections.length, 0);
  assert.equal(plan.unrepairable[0].reason, "no_valid_candidate");
  assert.deepEqual(
    plan.unrepairable[0].rejected.map((entry) => `${entry.source}:${entry.outcome}`),
    ["vps-pool:invalid", "vps-auth-json:inconclusive"]
  );
});

Deno.test("planCodexAuthRepair decides each slot independently", () => {
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, account("acct-a", NOW_MS + 3_600_000), valid(), true), localAccount(2, account("acct-b", NOW_MS + 3_600_000), invalid())],
    candidates: [
      assessed(candidate("vps-pool", "acct-a", NOW_MS + 86_400_000, { refresh: "other" }), valid()),
      assessed(candidate("vps-pool", "acct-b", NOW_MS + 86_400_000), valid()),
    ],
    nowMs: NOW_MS,
  });

  assert.deepEqual(
    plan.selections.map((entry) => entry.slot),
    [2]
  );
  assert.deepEqual(plan.healthy, [{ slot: 1, account_id: "acct-a", refresh_diverged: true }]);
});

Deno.test("applyCodexAuthPlan replaces only the affected account and preserves the rest", () => {
  const untouched = account("acct-a", NOW_MS + 3_600_000);
  const broken = account("acct-b", NOW_MS + 3_600_000);
  const pool = poolOf([untouched, broken]);
  const replacement = candidate("vps-pool", "acct-b", NOW_MS + 86_400_000, { refresh: "refresh-from-vps" });
  const plan = planCodexAuthRepair({
    accounts: [localAccount(1, untouched, valid()), localAccount(2, broken, invalid())],
    candidates: [assessed(replacement, valid())],
    nowMs: NOW_MS,
  });

  const applied = applyCodexAuthPlan(pool, plan, NOW_MS);
  assert.ok(applied);
  assert.equal(applied.accounts.length, 2);
  assert.equal(applied.accounts[0].access_token, untouched.access_token);
  assert.equal(applied.accounts[0].refresh_token, untouched.refresh_token);
  assert.deepEqual(
    applied.accounts.map((entry) => entry.account_id),
    ["acct-a", "acct-b"]
  );
  assert.equal(applied.accounts[1].access_token, replacement.access_token);
  assert.equal(applied.accounts[1].refresh_token, "refresh-from-vps");
  assert.equal(applied.accounts[1].updated_at_ms, NOW_MS);

  // The stored pool is replaced by a new value; the caller holds the old shape.
  assert.notEqual(applied, pool);
  assert.equal(pool.accounts[1].access_token, broken.access_token);
});

Deno.test("applyCodexAuthPlan with an empty plan returns an equivalent pool", () => {
  const pool = poolOf([account("acct-a", NOW_MS + 3_600_000)]);
  const plan = planCodexAuthRepair({ accounts: [localAccount(1, pool.accounts[0], valid())], candidates: [], nowMs: NOW_MS });
  const applied = applyCodexAuthPlan(pool, plan, NOW_MS);
  assert.ok(applied);
  assert.deepEqual(applied.accounts, pool.accounts);
});

Deno.test("codexAuthCandidateFromAuthJson accepts a real auth.json shape and rejects junk", () => {
  const parsed = codexAuthCandidateFromAuthJson(
    {
      auth_mode: "chatgpt",
      tokens: {
        access_token: tokenExpiringAt(NOW_MS + 3_600_000),
        refresh_token: "refresh-a",
        account_id: "acct-a",
      },
      last_refresh: "2026-09-12T21:16:41.259598Z",
    },
    "vps-auth-json",
    NOW_MS
  );
  assert.ok(parsed);
  assert.equal(parsed.account_id, "acct-a");
  assert.equal(parsed.refresh_token, "refresh-a");
  assert.equal(parsed.updated_at_ms, NOW_MS);

  assert.equal(codexAuthCandidateFromAuthJson({ tokens: {} }, "vps-auth-json", null), null);
  assert.equal(codexAuthCandidateFromAuthJson(null, "vps-auth-json", null), null);
  assert.equal(codexAuthCandidateFromAuthJson({ tokens: { access_token: "a", refresh_token: "b" } }, "vps-auth-json", null), null);
});

Deno.test("codexAuthCandidatesFromPool reads every stored account and rejects malformed pools", () => {
  const pool = poolOf([account("acct-a", NOW_MS + 3_600_000), account("acct-b", NOW_MS + 7_200_000)]);
  const candidates = codexAuthCandidatesFromPool(pool, "vps-pool");
  assert.deepEqual(
    candidates.map((entry) => `${entry.source}:${entry.account_id}`),
    ["vps-pool:acct-a", "vps-pool:acct-b"]
  );

  assert.deepEqual(codexAuthCandidatesFromPool(null, "vps-pool"), []);
  assert.deepEqual(codexAuthCandidatesFromPool({ accounts: [] }, "vps-pool"), []);
  assert.deepEqual(codexAuthCandidatesFromPool({ accounts: [{ account_id: "only-an-id" }], updated_at_ms: NOW_MS }, "vps-pool"), []);
});

Deno.test("codexAuthCandidateKey deduplicates the same credential seen from two sources", () => {
  const first = candidate("local-auth-json", "acct-a", NOW_MS + 3_600_000);
  const second = { ...first, source: "vps-pool" as const };
  const other = candidate("vps-pool", "acct-a", NOW_MS + 7_200_000);
  assert.equal(codexAuthCandidateKey(first), codexAuthCandidateKey(second));
  assert.notEqual(codexAuthCandidateKey(first), codexAuthCandidateKey(other));
});
