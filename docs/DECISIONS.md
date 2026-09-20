# ai.ubq.fi Decisions

Read before changing model routing, VPS acceptance, lint configuration, or Deno app inventory. These are scoped
decisions, not general policy; they narrow global defaults only for this repository and never weaken higher authority.

Provider routing decisions are maintained separately in `docs/provider-decision-journal.md`.

## Cache-read telemetry is reported, never defaulted - 2026-09-19

Relay the upstream cache-read counter when the provider publishes one; report an absent counter as unknown
(`usage_telemetry_status: "partial"`), never as a measured zero. DeepSeek publishes `usage.prompt_cache_hit_tokens`,
which this gateway relays as the official `prompt_tokens_details.cached_tokens` / `input_tokens_details.cached_tokens`
field so one reader serves either spelling.

Reason: every DeepSeek response previously hard-coded a cache read of zero, so the gateway and the Codex client both
reported a 0% cache hit rate for a caching feature that was working. That default erased the measurement the user asked
for and made a healthy upstream look unoptimized; a missing measurement and a measured zero are different facts and must
not share one value. A reported count larger than the request's own input is impossible, so it is dropped as unknown
rather than published.

Provider cache _control_ stays unsupported for DeepSeek, and that is separate from telemetry: DeepSeek's context cache
is automatic, prefix-based, and always on, with no client-facing request to enable, disable, pin, or expire it, so the
gateway still refuses `prompt_cache_options` and explicit breakpoints instead of forwarding parameters the upstream
cannot honor. Absent cache control is not absent caching.

Reversal risk: defaulting an unreported cache read to zero, publishing `prompt_tokens_details` when the upstream
reported no counter, or gating the metric on the model's `prompt_cache` catalog field, each reintroduces a number the
gateway cannot stand behind.

## Serial subscription routing - 2026-09-14

Exhaust one Codex subscription before moving ordinary requests to the next; do not spread them across both.

Reason: each subscription is a separate prompt-cache and account-health identity. Spreading ordinary requests across
both keeps neither cache warm, so latency and cost rise, and load lands on two accounts instead of one, which obscures
which subscription is degrading. The provider order Codex -> Surplus -> OpenLux is the intentional fallback chain.

Reversal risk: routing requests to a second subscription while the first still has capacity, or reordering the provider
chain, undoes this and reintroduces the cache and account-health problem. Do not "balance" load across subscriptions or
make the order dynamic without a new dated decision.

Implementation, not new verification: PR #307, deployed 2026-09-15 as 8bf9daad53a22abf8db4488ef1db2ac2d25c9d34, with
three accepted authenticated VPS-origin inference requests. Concurrent-admission follow-ups remain issues #308/#309.

## Serial-depletion VPS acceptance - 2026-09-15

For this named goal, the user excluded the offline Mac and confirmed VPS deployment. Use authenticated VPS-origin
inference, superseding this delivery's Mac-only handoff requirement. Preserve exact release checks, bounded inference
budget, and reset-credit safeguards.

## Lint migration - 2026-09-12

Use Prettier width 160 and cover all tracked source, tests, scripts, benchmarks, ops, and serve.ts. Fix the full finding
set, including behavior-preserving test fixes, rather than adding a suppression baseline. Leave CI unchanged for now.
Preserve the existing chore/lint-stack worktree.

Decision evidence: DSH session-8d0f4b3a-9ab5-43e1-96e6-fd092c509c26, recovered during the 2026-09-13 Codex takeover.

## Deno inventory cleanup - 2026-09-14

For the recorded cleanup, delete unused p-ai-ubq-fi and ai-ubq-fi-feat-shared-admin-toke. Retain ai-ubq-fi and
ubiquity-prospector; the Prospector monorepo's own `DECISIONS.md` owns the latter's retention and migration decisions.
