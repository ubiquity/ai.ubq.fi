# sub2api internal reliability gap audit handoff — 2026-08-21

## Status

Planning and source reconnaissance are complete. This handoff authorizes a read-only, owner-operated reliability gap
audit. It does not authorize implementation, tests, live inference, deployment, provider quota consumption, KV mutation,
process changes, a Git branch or worktree, a commit, a push, or a pull request.

## Objective

Compare the small internal reliability patterns in `Wei-Shaw/sub2api` with the current `ai.ubq.fi` implementation and
identify only confirmed, material gaps that benefit a gateway operated by one owner.

The audit must answer:

1. Which relevant sub2api reliability invariants are already present in ai.ubq.fi?
2. Which are absent or weaker in a way that can cause a concrete failure?
3. What is the smallest safe change for each confirmed gap?
4. Which sub2api patterns should be rejected because they exist for a public marketplace, conflict with current API
   contracts or privacy boundaries, or are unsuitable for Deno Deploy?

Success is a source-evidenced gap report with no speculative implementation backlog. Zero confirmed gaps is an
acceptable result.

## Current continuation state

- Repository: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Required checkout: the existing repository root above
- Branch at handoff: `development`
- HEAD at handoff: `3cc9bb121f65a8c88861d7708323935577833e09`
- Upstream tracking branch: `origin/development`
- External comparison repository: `https://github.com/Wei-Shaw/sub2api.git`
- Audited sub2api commit: `f646a1f974c26152160ef8327a7d6b9e3488ee83`
- Reconnaissance clone used by the planner: `/tmp/sub2api-audit.pChIwd/sub2api`
- Runtime and production state: not inspected for this audit; do not infer live or deployed behavior from source
- Tests: not run
- Repository files: no implementation file was changed for the reconnaissance; this handoff is the only file created for
  this task

The checkout was dirty before this handoff. Final verification showed modifications to:

- `scripts/stage0-cache-telemetry-gate.ts`
- `serve.ts`
- `src/codex.ts`
- `src/handler.ts`
- `src/metered.ts`
- `src/openai.ts`
- `src/paid_fallback.ts`
- `src/paid_fallback_ledger.ts`
- `src/prompt_cache_telemetry_gate.ts`
- `src/provider_health.ts`
- `src/surplus.ts`

It also included the untracked `docs/provider-reliability-efficiency-handoff-2026-08-21.md`. Treat all of this as
user-owned and potentially authoritative current reliability work. Recheck status and HEAD at continuation. Do not
reset, stash, clean, overwrite, commit, or move these changes. Do not create a clean worktree for this audit because
that would omit the current uncommitted behavior being evaluated.

This is a direct, single-writer continuation. The request lifecycle, routing circuits, failover, health, and settlement
surfaces overlap heavily and should not be split among implementation agents. Read-only parallel research is not needed
for this bounded comparison.

## Product boundary

ai.ubq.fi is operated by one owner. It is not being designed as a public AI marketplace or reseller platform.

Exclude all sub2api features whose primary purpose is multi-tenant commercialization or public operation:

- registration, user profiles, balances, plans, subscriptions, orders, payments, refunds, redemptions, and affiliates;
- customer groups, customer-specific pricing multipliers, profit reporting, and public model or channel plazas;
- user-level concurrency queues, public dashboards, announcements, content moderation, and prompt inspection;
- PostgreSQL and Redis infrastructure introduced to support those features;
- generalized native Anthropic, Gemini, Grok, Bedrock, Vertex, Kimi, Zhipu, or DeepSeek product expansion;
- marketplace-grade weighted scheduling unless a concrete current ai.ubq.fi failure requires it.

Do not turn excluded features into recommendations under a different name.

## Required references

Read these before drawing conclusions:

### ai.ubq.fi

- `AGENTS.md`
- `docs/provider-reliability-efficiency-handoff-2026-08-21.md`
- `docs/incident-codex-upstream-degradation-2026-08-21.md`
- `src/codex.ts`
- `src/codex_account_routing.ts`
- `src/inference_deadline.ts`
- `src/responses_stream.ts`
- `src/responses_failover_stream.ts`
- `src/openai.ts`
- `src/paid_fallback.ts`
- `src/paid_fallback_ledger.ts`
- `src/provider_health.ts`
- `src/metered.ts`
- `src/surplus.ts`
- `src/handler.ts`
- `src/analytics.ts`
- `src/types.ts`

Read current diffs for every dirty file in scope. A source file at HEAD is not the current implementation when it has
uncommitted changes.

### sub2api at the fixed comparison commit

- `backend/internal/service/openai_proxy_stream_circuit.go`
- `backend/internal/service/openai_stream_read_error.go`
- `backend/internal/service/openai_responses_rejected_field_retry.go`
- `backend/internal/service/openai_account_scheduler.go`
- `backend/internal/service/account_scheduling_threshold_eval.go`
- `backend/internal/service/concurrency_service.go`
- `backend/internal/service/openai_gateway_usage_integrity.go`
- `backend/internal/service/gateway_usage_billing.go`
- `backend/internal/service/channel_monitor_v2_error_taxonomy.go`
- `backend/internal/service/upstream_billing_probe.go`
- `backend/internal/service/scheduler_cache.go`
- `backend/internal/service/scheduler_snapshot_service.go`

If the temporary clone is absent, make another disposable clone under `/tmp` and check out the exact audited commit. Do
not add sub2api as a submodule unless the user separately requests a persistent dependency. Use source ideas as
evidence, not copied code; sub2api is LGPL-3.0.

## Existing ai.ubq.fi baseline to verify

Do not assume these reconnaissance conclusions are still correct. Verify them against the current dirty checkout:

- Codex is primary for known Codex models.
- Eligible failures enter a bounded paid-fallback waterfall.
- Surplus-primary models prefer Surplus then Metered; other models prefer Metered then Surplus.
- Paid fallback reserves exposure before transport and reconciles terminal usage durably.
- Unknown provider balance stays unknown; catalog reachability is not inference health.
- Codex account routing already has durable quota and upstream-timeout circuits, fenced half-open probes, generation
  checks, and reset reconciliation.
- Responses failover buffers pre-semantic events and prevents replay after semantic commitment.
- Streaming has distinct response-header or first-event, inactivity, and semantic-output deadlines.
- Stream ownership guarantees at most one client-visible terminal event.
- Provider health is passive and correlation IDs are sanitized.
- Terminal telemetry distinguishes provider attempts, fallback reason, stream outcome, provider request ID, and billing
  state.

If any baseline statement is false, show the exact path and line evidence and classify that discrepancy before comparing
sub2api.

## Audit invariants

Evaluate the following five areas. For each one, classify ai.ubq.fi as `equivalent-or-stronger`, `not-applicable`,
`confirmed-gap`, or `uncertain`, and provide direct source evidence from both repositories.

### A. Correlated streaming failures

sub2api collapses multiple stream disconnects occurring within a short interval on the same proxy or multiplexed HTTP/2
connection into one circuit failure. This prevents one shared transport incident from tripping a breaker as if several
independent failures occurred.

Determine whether ai.ubq.fi has a shared failure identity for which one network event can generate several circuit
votes. Inspect account scope, provider scope, HTTP connection reuse, concurrent streams, and circuit counters.

- Do not recommend burst collapsing merely because sub2api has it.
- A confirmed gap requires a plausible current path where correlated symptoms incorrectly advance one durable circuit
  more than once.
- If each failure affects an independent account circuit or a circuit opens from a single explicit timeout rather than a
  threshold, classify this as not applicable.
- Any proposed collapse key must avoid raw URLs, credentials, account IDs in public telemetry, and unbounded in-memory
  state.

### B. Billing and terminal-usage integrity

Trace each provider from admission through dispatch, semantic commitment, terminal event, cancellation, ambiguous
termination, settlement, reconciliation, health recording, and terminal telemetry.

Required invariant: a nominal HTTP or stream success without trustworthy billable usage must not become reconciled
billing evidence. It must become one of `not_billed`, `pending`, `unresolved`, or an equivalent truthful state based on
the provider contract. A missing usage field must not fabricate zero cost or provider health.

Check Codex, Metered, Surplus, and Cerebras separately. Their billing evidence contracts differ. Do not impose sub2api's
Grok-specific rule on providers that settle from a separate durable usage log.

### C. Request-wide attempt and retry bounds

Build the complete attempt graph for Chat Completions and Responses:

- retries within one provider or account;
- Codex sibling-account attempts;
- compatibility transformations;
- paid-provider fallback;
- pre-semantic stream retry;
- half-open recovery probes.

Prove that one inbound request has a finite upper bound and cannot repeat the same ineffective request transformation or
provider attempt indefinitely. Distinguish deliberate account retry from replay after semantic commitment.

sub2api's body-hash loop guard is only a reference pattern. Do not copy its behavior that removes upstream-rejected
fields dynamically. ai.ubq.fi must keep official OpenAI fields and explicit Codex compatibility extensions aligned with
the project contract. Recommend a shared retry budget only if the present composed paths lack an effective bound.

### D. Circuit capacity starvation and recovery

Verify behavior when every primary Codex account is withheld by quota, timeout, auth, or probe state.

Required invariants:

- no broad circuit bypass;
- at most one correctly fenced half-open probe for the applicable circuit generation;
- an eligible paid fallback may proceed only after its existing admission and budget gates;
- stale local state cannot overwrite a newer durable deadline or reset generation;
- an unavailable fallback returns a truthful bounded failure;
- recovery clears only the exact failure observation it proves healthy.

Compare sub2api's quarantine fail-open behavior, but do not adopt it if ai.ubq.fi's fenced probe plus paid fallback is
safer and already complete.

### E. Transport error taxonomy and evidence

Verify that ai.ubq.fi distinguishes, without leaking raw upstream data:

- client cancellation;
- gateway response-header or first-event deadline;
- stream inactivity deadline;
- upstream HTTP/2 reset or generic stream read failure;
- premature EOF before semantic output;
- failure after semantic commitment;
- valid provider terminal failure;
- malformed or oversized upstream error bodies.

Check how each class affects retry eligibility, circuit state, provider health, settlement, the client-visible error,
and terminal telemetry. A different error label alone is not a gap. A confirmed gap must change an operational decision
or conceal evidence required to diagnose a real failure.

## Secondary checks

Perform these only if they directly support one of the five audit areas:

- whether simultaneous requests can consume the same paid budget reservation;
- whether provider health can be promoted by non-inference activity;
- whether a successful terminal event can be counted or settled twice;
- whether a cancelled request can be classified as an upstream failure;
- whether a provider request ID is bounded and safe for logs and headers;
- whether an active fallback canary would prove the real incident-shaped request.

Do not expand this into a general security, performance, architecture, or code-quality audit.

## Decision standard

A `confirmed-gap` must include all of the following:

1. Exact ai.ubq.fi source path and relevant line or symbol.
2. The reachable failure sequence.
3. The incorrect user-visible, routing, health, or accounting outcome.
4. The sub2api pattern that demonstrates a viable invariant, with an exact commit-pinned source link.
5. The smallest ai.ubq.fi-native correction that preserves Deno Deploy and Deno KV.
6. A real acceptance surface and a focused regression check.
7. Any live, paid, deployment, test, or external-state action that requires approval.

Do not call something a gap because sub2api has more configuration, more abstractions, more dashboards, or more code. Do
not recommend generalized scoring or a new subsystem when an existing explicit state machine can enforce the invariant.

## Required output

Write the audit report to:

`/Users/nv/repos/ubiquity/ai.ubq.fi/docs/sub2api-internal-reliability-gap-audit-2026-08-21.md`

The report must contain:

1. Executive conclusion suitable for the owner.
2. Exact comparison commits and dirty-state caveat.
3. A five-row invariant matrix with classifications and source evidence.
4. Detailed findings ordered by operational risk, not by sub2api file order.
5. A rejected-patterns section explaining why excluded marketplace or infrastructure ideas do not belong here.
6. A minimal recommendation for each confirmed gap.
7. A separate `No action recommended` section for equivalent or non-applicable patterns.
8. An explicit implementation decision gate.

The implementation decision gate must stop after the report. It must ask the user before editing source, adding or
running tests, issuing live inference, consuming provider quota, mutating KV, deploying, pushing, or changing an
external monitor. Do not include a pre-authorized implementation phase in this handoff.

## Validation matrix

| Surface           | Required evidence                                                                                            |
| ----------------- | ------------------------------------------------------------------------------------------------------------ |
| Git identity      | Existing repository root, `development`, current HEAD, and full dirty status recorded before and after       |
| sub2api identity  | Exact commit `f646a1f974c26152160ef8327a7d6b9e3488ee83` verified locally                                     |
| Source comparison | Exact paths, symbols, and commit-pinned GitHub links for every material sub2api claim                        |
| Current behavior  | Dirty working-tree source and diffs included; HEAD-only inspection is insufficient                           |
| Request lifecycle | One explicit state sequence for every confirmed gap                                                          |
| Privacy           | No prompts, tool definitions, request or response bodies, credentials, or cache-key values captured          |
| Runtime           | Not required for the read-only audit; label all runtime conclusions as unverified unless separately approved |
| Tests             | Do not add or run tests during the audit; specify focused tests only as future acceptance criteria           |
| Live/provider     | No probe or inference; passive source evidence cannot be presented as live provider proof                    |
| Deployment        | None; do not claim production behavior or readiness                                                          |

## Safety boundaries

- Preserve all dirty, untracked, worktree, branch, process, provider, and external state.
- Do not terminate, restart, signal, or replace any process.
- Do not prune the many existing worktrees, including entries marked prunable.
- Do not add a submodule or persistent external dependency.
- Do not add an environment variable, secret, CLI flag, or configuration option.
- Do not run tests without asking the user first.
- Do not use a provider key or send a live inference request.
- Do not read or display credentials.
- Do not mutate Deno KV, deployment state, external monitoring, GitHub, or provider accounts.
- Do not log or persist request content.
- Do not weaken OpenAI schema compatibility or Codex CLI compatibility.
- Do not translate explicit reasoning `none` to omission or `null`; keep `ultra` to upstream `max` translation intact.

## Risks and known uncertainties

- The dirty checkout contains ongoing reliability work and may change after handoff. Reconcile it before conclusions.
- The planner did not establish current production identity or provider behavior for this comparison.
- sub2api runs a long-lived Go service with Redis and PostgreSQL. In-process circuits, background workers, leases, and
  cache snapshots may be invalid patterns for stateless Deno Deploy isolates.
- Similar names do not prove equivalent semantics. Compare reachable state transitions, not filenames.
- sub2api's public-marketplace requirements can make a correct local pattern appear more general than ai.ubq.fi needs.
- A live fallback canary may eventually be useful, but it consumes shared quota and requires separate approval.

## Completion requirements

Before reporting the audit complete:

- read this handoff and every required reference in full;
- record refreshed Git and sub2api identities;
- classify all five invariants;
- support each material statement with direct current-source evidence;
- distinguish source evidence from local runtime, live provider, deployed, and production evidence;
- confirm that no source implementation, tests, live requests, KV writes, deployments, process actions, pushes, or
  external mutations occurred;
- report the final dirty state without claiming ownership of unrelated changes;
- stop at the implementation decision gate.

## Successor continuation sentence

Read `/Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md` and
`/Users/nv/repos/ubiquity/ai.ubq.fi/docs/sub2api-internal-reliability-gap-audit-handoff-2026-08-21.md` in full, act as
the read-only planning facilitator on the existing dirty `development` checkout at `/Users/nv/repos/ubiquity/ai.ubq.fi`,
audit only the owner-operated internal reliability invariants against sub2api commit
`f646a1f974c26152160ef8327a7d6b9e3488ee83`, write the required evidence-backed gap report, preserve all user-owned
state, and stop for approval before any implementation, test, live inference, KV mutation, deployment, push, or external
change.

## Required final report to the user

Report:

- the audit report's absolute path;
- the ai.ubq.fi and sub2api identities used;
- the five invariant classifications;
- confirmed gaps and rejected recommendations;
- every evidence category actually obtained;
- every action not performed;
- final Git status and remaining uncertainty;
- the explicit question asking whether to implement any confirmed gap.
