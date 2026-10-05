# Incident: capacity-observed quota exhaustion dead-ended automatic banked resets (2026-10-05)

## Summary

Automatic banked-reset redemption had been unreachable since 2026-09-07. When a Codex subscription's requested quota
class was exhausted only through the capacity sampler (weekly window at 100% used, no live per-class circuit), routing
refused requests locally with `429 codex_quota_blocked` and never dispatched upstream. No fresh 429 could therefore
re-arm the reset fences, the blocked cohort was always empty, and the gateway silently continued on OpenAI's overage
credits instead of redeeming a banked reset. Three source fixes restored the path and were verified live on the VPS the
same night.

## Impact

- Automatic banked resets unused for ~4 weeks despite repeated exhaustion; six credits were approaching expiry.
- Requests intermittently refused with a local `429` while upstream still served via overage credits.
- No data loss. No unauthorized external calls. All redemption attempts were driven by ordinary gateway inference
  requests.

## Timeline (UTC)

- Before incident: deployed release `987aada1`; development `88b2c2be`; last verified redemption 2026-09-07.
- 03:32-03:50 Diagnosis: journal showed local `codex_quota_blocked` with zero upstream dispatch and zero `codex_reset_*`
  telemetry in 30 days; a read-only KV probe showed both slots at 100% used, `banked_reset_generation_ambiguous=true`,
  no class blocks, `blockedAccounts=[]`.
- 03:53 Fix 1 deployed: `fe8c8fa1` (PR #909), capacity-exhausted accounts admit a bounded half-open probe; the probe
  lease is mintable without a class-block deadline; empty-identity preflights are logged.
- 03:54-03:59 Direct quota checks: 3 reset credits per account (6 total); nearest expiries `2026-10-05T04:20:26Z`
  (pavlovcik) and `2026-10-05T04:20:53Z` (ubiquitydao); weekly windows 100% used; both accounts still served via overage
  credit balances.
- 04:01:52-55 ubiquitydao redemption, automatic, through the public gateway:
  `codex_reset_eligible -> live_armed -> claimed -> submitted -> verified` (`redeem_outcome:"reset"`); credit count 3 ->
  2; upstream weekly window 100% -> 0%.
- 04:09 Fix 2 deployed: `bfe1375f` (PR #911), a 100%-used capacity observation with a future reset deadline now forms
  the blocked cohort identity, and the blocked-cohort evaluator materializes the durable class block so an ordinary
  request drives arm and spend.
- 04:10:20-23 pavlovcik, automatic: `live_armed -> claimed`, then `skipped - reason:"global_limit_reached"`. The
  once-per-UTC-day global cap had been consumed by ubiquitydao at 04:01:52
  (`global_day 2026-10-05: submission_count 1`).
- 04:12-04:16 Governance repair: `6456231a` (PR #913 lint), `0f3fb00e` (PR #912 regression tests); development CI red on
  two regression assertions that pin the interim probe behavior; formatter output merged as `984572ae` (PR #914).
- ~04:20 The owner used pavlovcik's expiring credit manually before it lapsed.

## Root cause

1. `evaluateCodexRoutingAccount` treated a capacity-observed exhaustion as a skip with **no blocked identity**, so
   `evaluateBlockedCohortBankedReset` saw `blockedAccounts.length === 0` and returned silently, while
   `quotaBlockedOrRetryableResponse` answered the client locally.
2. Because the account was never dispatched in that state, no authoritative upstream
   `429 (usage_limit_reached, resets_at)` could ever re-establish a claimable fence.
3. The `banked_reset_generation_ambiguous` slot flag is sticky and is only cleared by a successful claimed probe; a
   capacity-exhausted account with no class block could not claim a probe, so the flag stayed set and the claim fence
   (`isCodexQuotaBlockFenceCurrent`, which requires `!ambiguous`) refused every spend.

## Fix

- `src/codex/routing-evaluation.ts`: a fresh 100%-used observation with a future reset deadline now yields a blocked
  identity (`quotaResetAtMs` = the observed reset); without a deadline the account stays half-open as a bounded probe.
- `src/codex/routing-mutations.ts`: `buildExpiredProbeClaim` mints a lease for a capacity-exhausted quota probe with no
  class block (`quotaHeadroom === 0`), because a successful probe is the only trusted way to clear reset ambiguity.
- `src/codex/rate-limit-429.ts`: `liveDeadlineSupersedes` lets a fresh stable future deadline from a live 429 supersede
  an absent or expired prior observation; the durable ledger still makes every quota window spend-once.
- `src/codex/responses-operations.ts`: the blocked-cohort evaluator materializes the durable class block from the fresh
  exhausted observation (same path a real 429 would take) before evaluating, re-runs the strong selection once, and logs
  `codex_banked_reset_preflight {outcome:"skipped", reason:"no_blocked_identity"}` instead of failing silently.

## Live acceptance evidence

- ubiquitydao: `codex_reset_verified` with `redeem_outcome:"reset"`; upstream weekly window 100% -> 0%; available_count
  3 -> 2; a standard inference call served HTTP 200 immediately after.
- pavlovcik: candidate -> `live_armed` -> `codex_reset_claimed` fired automatically; the spend was refused solely by the
  global daily cap, not by any fence defect.
- Per-account end-to-end calls through the public gateway (`https://ai.ubq.fi`) were pinned by subscription selection;
  selection was restored to all five providers afterwards. No OpenAI endpoint was called directly to trigger a reset at
  any point.

## What went wrong in the response

- Sequencing error under the once-per-day global cap: redeeming ubiquitydao first consumed the day's single redemption,
  which made a gateway redemption for pavlovcik impossible that day (at most one was possible). The earlier-expiring
  account (pavlovcik) should have been chosen first, or the choice should have been confirmed with the owner.
- Two of the merged regression tests pin the interim probe behavior and fail against the final contract; development CI
  is red on those two assertions until a test-only update lands.
- Emergency merges were admin-merged ahead of CI; process debt was repaired the same night (lint, tests, formatter)
  except for the two assertions above.

## Open items

- Test-only: update the two failing assertions in `tests/codex-banked-reset-capacity-recovery.test.ts` to the final
  blocked-identity contract.
- Approved, not implemented: replace the global once-per-day cap with one redemption per account per UTC day.
- Approved, not implemented: an explicit per-account overage-usage setting (default off; prefer banked resets; allow
  overage only when no banked resets remain).
- The incident mock-suite companion file was not finished (work stopped by owner instruction).

## Appendix: identifiers

- Releases: `987aada1` (at incident) -> `fe8c8fa1` -> `bfe1375f` (live during acceptance) ; development tip after
  repairs `0f3fb00e`, formatter `984572ae`.
- PRs: #909 (probe), #910 (materialization v1), #911 (capacity block identity), #912 (regression tests), #913 (lint),
  #914 (formatter).
- Remote paths: a reused capacity-identified `gpt-reserve` 429 opened the redemption window for ubiquitydao; the daily
  cap record `["uos_ai","codex_reset_redemption","global_day","v1","2026-10-05"]` carries `submission_count: 1` at
  `updated_at_ms 1791172912427`.
