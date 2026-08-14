# `ai.ubq.fi` usage-optimization plan

**Date:** 2026-08-09\
**Canonical repository:** `/Users/nv/repos/ubiquity/ai.ubq.fi`\
**Canonical branch:** `development`\
**Live revision at planning time:** `3639d611b9ec9ad3d5bd8f2a538df65375e2a47d`

This document is an agent handoff. Agents must preserve unrelated dirty files, must not deploy or change production
without approval, and must return evidence that is tied to an exact commit.

## Objective

Keep the service comfortably inside Deno Deploy Pro while reducing avoidable usage. Treat Free-tier migration as a
separate, harder objective; do not weaken authentication, quota admission, ledger settlement, or official Codex request
semantics only to chase Free limits.

The first success condition is:

- Pro egress stays below 200 GB/month at the measured production trend.
- Pro KV reads stay below 1.3M/month and writes below 0.9M/month, or the remaining overage is explicitly accepted.
- Requests, CPU, memory time, active apps, error rate, latency, streaming, and upstream behavior do not regress.

The optional Free objective is stricter: 1M requests, 15 CPU hours, 350 GB-h memory time, 20 GB egress, 450k KV reads,
300k KV writes, and at most 20 active apps per month.

## Verified baseline

The official [Deno Deploy pricing page](https://deno.com/deploy/pricing/) currently lists these Pro allowances and
overage rates:

| Metric      |     Pro included |     Overage |
| ----------- | ---------------: | ----------: |
| Requests    |         5M/month |        $2/M |
| CPU time    |       40 h/month |     $0.05/h |
| Memory time | 1,000 GB-h/month | $0.016/GB-h |
| Egress      |     200 GB/month |    $0.50/GB |
| KV reads    |       1.3M/month |        $1/M |
| KV writes   |       0.9M/month |     $2.50/M |
| Active apps |              100 |           — |

Billing snapshot from 2026-08-09:

| Metric       | Current cycle | Interpretation                 |
| ------------ | ------------: | ------------------------------ |
| Requests     |          0.3M | Within Pro                     |
| CPU time     |         9.4 h | Within Pro                     |
| Memory time  |   229.6 GiB-h | Within Pro                     |
| Egress       |        87 GiB | Within Pro                     |
| KV reads     |          3.3M | About 2.0M read units over Pro |
| KV writes    |          0.6M | Within Pro                     |
| Active apps  |            24 | Within Pro                     |
| Usage charge |   About $1.99 | Consistent with read overage   |

The fixed-end 28-day production projection is approximately 666k requests, 21.2 CPU hours, 176 GiB egress, 7.76M reads,
and 1.46M writes. At current Pro rates this is roughly $28/month including the $20 base fee. The previously reported
$115–$125 figure is only a hot-week stress run-rate; it must not be used as the expected bill without confirming that
the hot week repeats.

## Cost-saving targets

| Scenario                                           | Approximate effect                                                                                                                                 |
| -------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Remove projected Pro KV overage                    | Save about $7.86/month; projected bill approaches $20/month                                                                                        |
| Keep egress below Pro allowance during a hot trend | Avoid roughly $75–$90/month of egress overage if that trend actually repeats                                                                       |
| Reach Free limits                                  | Requires architectural changes: roughly 89% egress reduction and 94% read reduction on the smoothed trend, plus lower writes, CPU, and active apps |

Do not promise Free-tier success from a local unit test. Use the same fixed-end dashboard windows and a full production
cycle after rollout.

## Guardrails for every agent

- Do not use `DENO_DEPLOY_TOKEN` as a data-plane key or bypass admin checks.
- Do not silently remove, truncate, summarize, or rewrite official Codex input.
- Do not weaken V3 reservation, dispatch, release, idempotency, or CAS invariants.
- Do not remove `UOS_AI_TOKEN` allowlist isolation or turn it into a general shared production credential without an
  explicit security decision.
- Do not add environment variables, CLI flags, deploys, cron jobs, schema migrations, or external services without
  approval.
- Do not run broad stress tests against production. Reuse existing samples and persist results in the audit document.
- A focused test, `deno check`, or health response is not proof of streaming or production acceptance. Report those
  separately.

## Workstreams

Agents should begin with a read-only report and focused tests. Implementation work is sequential on the canonical branch
unless the primary agent approves truly disjoint ownership and integration order.

### A. Measurement and budget fixtures

**Owner surface:** `tests/`, test helpers, and this document only.\
**Purpose:** establish operation and byte baselines without adding production KV writes.

Tasks:

1. Add or extend a fake/counting `Deno.Kv` test double that records every command and atomic commit, grouped by auth
   kind and request outcome.
2. Cover bounded API keys, unlimited API keys, `UOS_AI_TOKEN`, admin tokens, upstream failures, retries, client
   disconnects, and concurrent admission.
3. Add a test-only request-body byte measurement around the Codex serialization boundary; do not emit it to production
   KV.
4. Produce a table of reads, writes, serialized request bytes, response bytes, and latency per scenario. Tie it to the
   tested commit.

**Acceptance:** the fixture distinguishes mandatory correctness operations from optional telemetry/background work and
does not require a live upstream call.

#### Test-only measurement baseline

The committed fixture is `tests/helpers/counting_kv.ts` and `tests/usage-optimization-measurement.test.ts`. It uses an
in-memory KV store, seeds outside the measured window, and intercepts the exact serialized string given to the Codex
transport. It makes no live upstream request and writes no production KV data.

Run:

```sh
deno test --allow-env tests/usage-optimization-measurement.test.ts
```

The command prints an operation and byte table. Counts are test-fixture commands and mutations, not Deno billing units;
an atomic commit remains visible separately. `response bytes` are the mocked upstream body bytes, not gateway egress.
Latency is a local fake-transport sample and is therefore a regression signal only, not a production latency claim.

The table below is the initial local sample. The test prints a fresh table for every run; concurrent admission totals
and local latency can vary with task scheduling, while the fixture assertions preserve the required route outcomes, wire
bytes, ledger separation, and durable-operation floors.

| Auth kind and outcome                        | Read commands | Write mutations | Atomic commits | Mandatory commands | Optional commands | Request bytes | Response bytes | Local latency ms |
| -------------------------------------------- | ------------: | --------------: | -------------: | -----------------: | ----------------: | ------------: | -------------: | ---------------: |
| Bounded API key, success                     |            13 |               6 |              3 |                 12 |                 2 |           209 |            178 |               74 |
| Unlimited API key, success                   |            13 |               6 |              3 |                 12 |                 2 |           209 |            180 |               28 |
| UOS allowlist, success                       |             5 |               2 |              1 |                  2 |                 2 |           209 |            174 |               32 |
| Admin allowlist, success                     |             5 |               2 |              1 |                  2 |                 2 |           209 |            176 |               16 |
| Bounded API key, upstream failure            |            13 |               6 |              3 |                 12 |                 2 |           209 |             74 |               17 |
| Codex auth pool, one retry                   |             4 |               4 |              2 |                  1 |                 3 |           180 |            254 |                7 |
| Bounded API key, client disconnect           |            13 |               6 |              3 |                 12 |                 2 |           209 |             63 |               12 |
| Bounded API key, eight concurrent admissions |           128 |               8 |              4 |                110 |                 8 |           209 |            181 |              123 |

Before this fixture, this worktree had no per-auth-kind command/atomic or Codex serialization-byte baseline. The
required implementation handoff ties this table to its exact tested commit. The fixture asserts that UOS and admin
allowlist paths do not access V3 API-key ledger keys, while the bounded paths retain durable reservation, dispatch, and
disconnect compensation operations.

### B. V3 ledger and authentication path

**Owner surface:** `src/api_key_policy.ts`, `src/auth.ts`, and directly related ledger tests.\
**Purpose:** reduce repeated reads while preserving strict quota correctness.

Tasks:

1. Map each read and atomic commit in authentication, admission, dispatch, and completion to the invariant it protects.
2. Verify which policy/config values can use existing bounded in-process caches and which values must be strongly
   reread.
3. Look for safe read coalescing or reuse within one request. Measure billed KV commands; do not assume a convenience
   API reduces read units.
4. Quantify the already-existing `UOS_AI_TOKEN` allowlist path separately. It may remove the API-key ledger cost, but it
   must retain shared-secret risk controls and terminal telemetry decisions.
5. Add concurrency and crash/retry tests before changing any ledger sequence.

**Acceptance:** no quota bypass, double settlement, lost reservation, stale policy decision, or cross-key accounting is
possible; the counting fixture shows a measurable read reduction.

### C. Optional telemetry and background work

**Owner surface:** `src/prompt_cache_telemetry_gate.ts`, terminal logging in `src/handler.ts`, `serve.ts`,
`src/paid_fallback_ledger.ts`, and `src/provider_capacity.ts`.\
**Purpose:** remove optional KV work without losing incident visibility or billing correctness.

Tasks:

1. Separate prompt-cache counters and diagnostic writes from admission and settlement writes.
2. Compare exact-once counters, bounded sampling, and log-only evidence. Keep enough signal to investigate upstream
   cache behavior.
3. Measure the per-minute reconciliation and 15-minute provider-capacity floors during quiet traffic.
4. Coalesce or reduce background work only when retries, lease ownership, billing settlement, and capacity safety remain
   provable.

**Acceptance:** a failed optional telemetry write cannot fail a request; paid fallback settlement and capacity decisions
remain durable and idempotent.

### D. Outbound byte reduction

**Owner surface:** `src/codex.ts`, `src/responses_stream.ts`, and protocol compatibility tests.\
**Purpose:** protect the 200 GB Pro egress allowance and investigate the Free blocker without changing official client
semantics.

Tasks:

1. Measure serialized upstream request-body size and streamed response size for the existing production-shaped samples.
   Keep inbound and outbound bytes separate.
2. Verify whether the upstream Codex endpoint accepts a standards-compliant compressed request body. Test
   authentication, retries, SSE, and error responses before considering compression.
3. Evaluate `previous_response_id` or client-side context compaction only when the official client and upstream protocol
   preserve the same conversation semantics. The gateway must not silently drop old turns.
4. Treat response compression as a separate experiment; never buffer or break SSE delivery to save bytes.

**Acceptance:** request/response meaning, stream timing, cancellation, and upstream compatibility remain unchanged; a
measured byte reduction is tied to a reproducible test.

### E. Production economics and monitoring

**Owner surface:** `docs/`, existing health/metrics paths, and deployment runbooks.\
**Purpose:** keep the service within an intentional monthly budget.

Tasks:

1. Define warning thresholds below the Pro limits (for example, 150 GB egress, 1.0M reads, and 0.7M writes) using
   existing observability surfaces before adding new configuration.
2. Re-run fixed-end 7/14/21/28-day samples after each accepted optimization.
3. Record Pro cost estimates separately from Free feasibility. Include current cycle, smoothed 28-day, and hot-week
   stress cases.
4. Keep a rollback note for every production change and record the exact live deployment SHA.

**Acceptance:** an operator can identify a rising egress or KV trend before an overage, and all estimates state their
sampling window and units.

## Integration order

1. Measurement fixtures and read-only ledger map.
2. Optional telemetry/background accounting reductions.
3. Safe V3 read coalescing or cache reuse.
4. Outbound compression or protocol experiments, one at a time.
5. Production rollout only after focused tests, `deno check`, and an approved deployment window.

There must be one canonical implementation writer during integration. Before assigning a writer, reconcile `git status`,
`git worktree list`, branch state, and any pending agent commits. Do not create competing ledger implementations or
compatibility fallbacks.

## Required handoff from each implementation agent

Return:

- exact base SHA and resulting commit SHA;
- owned files and a short behavior summary;
- focused tests and their output;
- operation/byte budget before and after;
- security, quota, streaming, and migration concerns;
- unresolved questions and a clear recommendation to integrate or reject.

No agent should claim Free-tier readiness from local evidence alone. The final decision requires a full-cycle production
measurement and an owner-approved cost/availability trade-off.
