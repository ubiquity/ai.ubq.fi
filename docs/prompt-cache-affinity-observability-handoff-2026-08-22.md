<!-- deno-fmt-ignore-file -->

# Prompt-cache affinity and observability handoff

## Authority and role

This document is the implementation authority for the next prompt-cache improvement goal in `ai.ubq.fi`. The next primary agent acts as the orchestrator. It must preserve the fixed provider waterfall, the official OpenAI wire schema, unrelated dirty work, and the approval boundaries below.

## Objective

Improve the probability and measurement of upstream prompt-cache reuse without claiming that the gateway owns or controls any provider cache.

The implementation must:

1. Keep an authenticated principal plus explicit `prompt_cache_key` on the same eligible ChatGPT Codex subscription account when possible.
2. Split durable prompt-cache analytics by bounded provider, model, route, key, mode, and fallback dimensions.
3. Verify and, only when supported by provider wire evidence, normalize paid-provider cache usage fields into the official OpenAI usage shape.
4. Preserve standard cache controls for Surplus and OpenLux/Metered while avoiding Codex-private transport headers on those providers.
5. Produce focused local evidence before any optional deployment or quota-consuming live probe.

## Definition of success

The goal is successful locally when all accepted worker tips are ancestors of the canonical goal branch, combined validation passes there, and the following behavior is proved with deterministic tests:

- Repeated keyed Codex requests from the same authenticated principal prefer the same still-eligible account.
- Quota, invalid credentials, or another authoritative account-health signal overrides affinity immediately.
- Unkeyed traffic and paid-provider ordering retain their existing behavior.
- Prompt-cache analytics can report token hit rate and request hit rate by each approved bounded dimension.
- Cache telemetry never stores a raw prompt, raw cache key, auth token, account identifier, request identifier, or arbitrary user metadata.
- Surplus or Metered native cache fields are normalized only if a captured fixture or primary provider contract proves their real wire names and semantics.

Deployment, promotion, and live provider probes are separate completion stages. They require current user approval. Local completion must not be described as deployed or live-accepted.

## Current authoritative state

- Repository root: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Clean source worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-analytics-integration`
- Branch: `development`
- Local and `origin/development` SHA at handoff creation: `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`
- Production Deno revision: `gajb29k3mssd`
- Production health identity previously verified: Git SHA `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`, revision `gajb29k3mssd`
- Live acceptance already obtained for `gpt-5.6-luna` through `x-uos-upstream: chatgpt_codex`: the second Chat Completions request and the second Responses request each reported `2,816 cached_tokens`.
- Historical production documentation also records successful keyed cache reuse for `gpt-5.6-terra`.
- The clean source worktree had no pending changes before this handoff file was added.
- The original repository checkout is user-owned, was previously reported dirty, and is on `fix/agent-readiness-p2` at `f9d9f88d787091671ffeb9432bf803d4dfa1531a`. Do not edit it, switch it, clean it, or use it as the implementation lane.

### Existing behavior that must be preserved

- `src/codex.ts` derives a stable UUID-shaped native subscription identity from authenticated scope plus the caller's nonblank `prompt_cache_key`.
- ChatGPT Codex receives that identity through `conversation_id`, `session-id`, `thread-id`, and `x-client-request-id`.
- Only the ChatGPT Codex subscription leg strips cache options, cache retention, explicit breakpoints, and unsupported output caps; it reports warnings.
- Surplus and OpenLux/Metered receive the canonical request body with official cache fields preserved.
- The fixed inference waterfall remains eligible ChatGPT Codex subscription capacity, then Surplus Intelligence, then OpenLux/Metered. Only authoritative quota or capacity signals advance the waterfall.
- Current terminal telemetry already contains final provider, model, route, key presence, cache mode, fallback reason, attempted providers, cached tokens, and cache-write tokens.
- Current durable `prompt_cache_analytics` uses one all-provider, all-model, all-route 15-minute bucket. It cannot compare cohorts or calculate request hit rate.
- Current standard terminal usage parsing reads `usage.input_tokens_details.cached_tokens` and optional `cache_write_tokens`.
- Surplus pricing metadata supports cache-read and cache-write prices. This is not proof of its terminal usage wire shape or cache capability.

## Canonical Goal Identity

- Canonical plan path: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-analytics-integration/docs/prompt-cache-affinity-observability-handoff-2026-08-22.md`
- Canonical goal identifier: `prompt-cache-affinity-observability-hand-g180c185110`
- Goal slug: `prompt-cache-affinity-observability-hand`
- Hash suffix: `g180c185110`
- Canonical worktree name: `prompt-cache-affinity-observability-hand-g180c185110`
- Repository root: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Canonical worktree path: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-g180c185110`
- Canonical branch: `codex/prompt-cache-affinity-observability-hand-g180c185110`
- Base ref: `development`
- Exact base SHA: `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`
- Lane state: `planned`; the successor orchestrator creates it only after proving that the recorded path and branch are absent and the base SHA is still available.
- Persistent goal sentence: Goal: Use canonical worktree name prompt-cache-affinity-observability-hand-g180c185110 at /Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-g180c185110 on branch codex/prompt-cache-affinity-observability-hand-g180c185110; read /Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md and /Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-analytics-integration/docs/prompt-cache-affinity-observability-handoff-2026-08-22.md in full, then act as orchestrator and implement the plan end to end, delegate each write module only to its recorded isolated worktree, keep integration and final validation in the canonical worktree, preserve the fixed provider waterfall and approval boundaries, and never switch the canonical branch or worktree.

The canonical plan path is the stable goal identifier. Do not move this file or recompute the lane after handoff.

## Scope

### A. Keyed Codex account affinity

Add a bounded, privacy-safe affinity record for authenticated requests that contain a nonblank official `prompt_cache_key`.

Required behavior:

- Derive the affinity identity from the authenticated principal and explicit cache key using a domain-separated digest. Never persist either raw input.
- Record only an opaque account-cohort hash and expiry. Do not persist raw subscription account identifiers.
- Use a short expiry no longer than the expected in-memory cache lifetime. Use Deno KV expiry instead of adding a new cleanup service.
- Prefer the recorded account only when it remains eligible under existing auth, quota, health, model, and banked-reset rules.
- On authoritative quota or auth failure, use existing routing rules immediately and update affinity only after a later account succeeds.
- Do not turn timeouts, stalls, network errors, read errors, or upstream 5xx responses into quota evidence.
- Do not let account affinity change the paid-provider waterfall or budget admission.
- Leave unkeyed requests on the current path. Do not generate a cache key from the full body or from a broad method-level identity.
- Use a bounded telemetry enum such as `preferred`, `preferred_unavailable`, `remapped`, and `none`; do not log the digest.
- Treat broad principals, including repository-wide or method-wide identities, conservatively. If a stable per-credential or per-user scope cannot be proved, do not persist account affinity for that request.

### B. Prompt-cache analytics v2

Use a hard cutover to a new KV namespace. Do not dual-write indefinitely or create a legacy compatibility layer. Old v1 entries may expire under their existing retention policy.

Allowed durable dimensions:

- final provider from the existing bounded provider enum;
- domain-separated model hash or another catalog-derived bounded model cohort;
- route: `responses` or `chat.completions`;
- `prompt_cache_key` present: boolean;
- cache mode from the existing bounded mode enum;
- bounded fallback class, with `none` as the normal value.

Required counters:

- input tokens;
- cached input tokens;
- cache-write input tokens when reported;
- sample count;
- request cache-hit sample count, where a hit means `cached_input_tokens > 0`;
- cache-write-reported sample count;
- sufficient coverage or invalid counters to distinguish zero from missing telemetry.

Required read behavior:

- Preserve the current 15-minute buckets, seven-day view, and eight-day retention unless a focused test proves a defect.
- Support a bounded `group_by` allowlist for provider, model, route, key presence, mode, and fallback.
- Aggregate every dimension not selected by `group_by`.
- Return token hit percentage, request hit percentage, cache reads per write when calculable, sample counts, and telemetry coverage.
- Cap response cardinality and reject unknown grouping values.
- Keep concurrent writes atomic.
- Keep the admin unavailable state explicit. Never render unavailable data as a real zero.

Required admin UI behavior:

- Show token hit rate and request hit rate separately.
- Provide bounded filters or grouping controls without exposing raw keys or request identifiers.
- Identify keyed versus unkeyed cache-eligible traffic so operators can find missed opportunities.
- Preserve the existing retention explanation and unavailable-state handling.

### C. Paid-provider cache usage normalization

First establish the provider's actual terminal usage shape from an existing sanitized fixture, existing bounded logs, or primary provider documentation. Do not infer terminal field names from pricing metadata alone.

If evidence confirms provider-native cache fields:

- Add a small pure internal normalizer that accepts only the proved Surplus or Metered upstream shape.
- Normalize it to official OpenAI response usage: `usage.input_tokens_details.cached_tokens` and optional `cache_write_tokens`.
- Preserve official fields when already present.
- Reject negative, fractional, unsafe, or internally inconsistent counts.
- Prevent duplicate billing or analytics when both official and provider-native fields appear.
- Add fixtures for standard fields, proved native fields, missing fields, zero values, malformed values, and mixed fields.

If no evidence confirms an alternate shape, make no speculative production parser change. Record that outcome in the final report and rely on the standard parser.

### D. Provider-neutral best effort

- Continue passing an explicit caller-supplied `prompt_cache_key` and validated official cache controls to paid providers.
- Keep normalized instructions, messages, tools, schemas, and their order deterministic.
- Keep request IDs, timestamps, and telemetry outside the prompt body.
- Do not reorder, strip, or rewrite user messages, tools, or schemas to chase a cache hit.
- Do not copy Codex-private session headers to Surplus or OpenLux/Metered.
- Do not auto-generate keys for unknown providers. A future generated-key contract requires a real stable conversation identifier and provider/model capability proof.
- Do not hold traffic on a more expensive provider only because it previously returned a cache hit.

## Non-goals

- Implementing a gateway-owned model inference cache.
- Storing prompt bodies or reusable prompt prefixes.
- Changing the public OpenAI request or response schema.
- Adding a gateway-only cache alias, cache sentinel, environment variable, secret, CLI argument, or provider-forcing request parameter.
- Enabling `previous_response_id`, provider-native stored Responses state, or cross-provider response-ID replay. The gateway currently uses `store: false`; provider-bound continuation needs a separate privacy and failover design.
- Changing prompt content or tool order.
- Reversing the fixed cost waterfall.
- Claiming paid-provider cache support from catalog membership, pricing metadata, or a successful request with zero cache tokens.
- Pushing, opening a PR, deploying, promoting a Deno revision, or running quota-consuming live probes without current user approval.

## Safety boundaries

- Preserve the original dirty checkout and every unrelated worktree.
- Never kill, restart, signal, detach, or replace a Codex, Deno, browser, SSH, tmux, or app-server process.
- Do not print or persist auth tokens, raw cache keys, prompt content, account IDs, or full provider responses.
- Do not use deploy credentials for inference probes. Use the least-privilege `UOS_AI_TOKEN` only after explicit probe approval.
- A live paid-provider probe must have an approved provider/model target, request count, and spend cap.
- A deployment must follow the repository's immutable revision identity checks and explicit Deno promotion contract. A successful build is not proof that the stable route moved.

## Shared integration contract

The orchestrator owns shared and conflict-prone surfaces, especially `src/openai.ts`, shared response telemetry types, final routing integration, combined tests, Git integration, and final acceptance.

Workers must not edit `src/openai.ts`. If a module needs a shared change there, it must return the exact requested integration patch description to the orchestrator. The orchestrator applies one combined change after integrating the worker tips.

Every accepted worker must return:

- module ID;
- exact base SHA and head SHA;
- clean status;
- changed files;
- validation commands and results;
- unresolved concerns;
- explicit statement that it did not push, deploy, run a live probe, or alter another worktree.

## Implementation modules

### m01-cache-analytics-v2

- Purpose: implement durable bounded cache analytics dimensions, hit counters, grouped reads, and admin rendering.
- Module hash: `af1b25e9fc5`
- Worker lane: `prompt-cache-affinity-observability-hand-m01-cache-analytics-v2-af1b25e9fc5`
- Worker branch: `codex/prompt-cache-affinity-observability-hand-m01-cache-analytics-v2-af1b25e9fc5`
- Worker worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-m01-cache-analytics-v2-af1b25e9fc5`
- Expected base: `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`
- Dependencies: none; may run concurrently with m02 and m03.
- Owned files: `src/prompt_cache_analytics.ts`, `src/handler.ts`, `src/admin.ts`, `static/admin.js`, their focused analytics/admin tests, and no others unless the orchestrator approves a specific dependency.
- Prohibited files: `src/openai.ts`, `src/codex.ts`, `src/codex_account_routing.ts`, `src/surplus.ts`, and `src/metered.ts`.
- Required validation: concurrent atomic writes, grouped aggregation, retention pruning, unavailable state, malformed telemetry, bounded cardinality, legacy v1 isolation, admin response validation, and UI rendering logic.
- Handback: include the exact analytics event fields that `src/openai.ts` or the coordinator must supply.

### m02-codex-account-affinity

- Purpose: keep explicit keyed requests on their prior successful eligible Codex account without weakening capacity or auth rules.
- Module hash: `add7defbe3c`
- Worker lane: `prompt-cache-affinity-observability-hand-m02-codex-account-affinity-add7defbe3c`
- Worker branch: `codex/prompt-cache-affinity-observability-hand-m02-codex-account-affinity-add7defbe3c`
- Worker worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-m02-codex-account-affinity-add7defbe3c`
- Expected base: `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`
- Dependencies: none; may run concurrently with m01 and m03.
- Owned files: `src/codex.ts`, `src/codex_account_routing.ts`, a new narrowly scoped affinity store module only if required, and focused Codex routing tests.
- Prohibited files: `src/openai.ts`, analytics/admin files, `src/surplus.ts`, and `src/metered.ts`.
- Required validation: same principal/key stability, principal separation, key separation, no-key unchanged behavior, preferred-account health checks, authoritative quota remap, transient failure behavior, expiry, concurrency, account removal, credential refresh, banked-reset compatibility, and no raw identifiers in storage or logs.
- Handback: specify any internal response header or result field that the orchestrator must map into existing `affinityOutcome` telemetry.

### m03-paid-cache-usage

- Purpose: prove the actual paid-provider terminal cache usage shape and implement a pure normalizer only when evidence supports it.
- Module hash: `a0237a9399e`
- Worker lane: `prompt-cache-affinity-observability-hand-m03-paid-cache-usage-a0237a9399e`
- Worker branch: `codex/prompt-cache-affinity-observability-hand-m03-paid-cache-usage-a0237a9399e`
- Worker worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-m03-paid-cache-usage-a0237a9399e`
- Expected base: `7ff7acd1a277b3dbab6169c74862c6d2b0de2a26`
- Dependencies: none; may run concurrently with m01 and m02.
- Owned files: `src/surplus.ts`, `src/metered.ts`, a new small provider-usage normalizer module if evidence requires it, sanitized fixtures, and focused paid-provider tests.
- Prohibited files: `src/openai.ts`, Codex routing files, and analytics/admin files.
- Required validation: official shape passthrough, confirmed native shape normalization, zero versus missing, malformed counts, mixed shape precedence, no double counting, streaming and non-streaming terminal handling where applicable.
- Evidence rule: if no primary or captured evidence proves alternate native terminal fields, return a clean no-change result with the evidence reviewed. Do not guess.
- Handback: give the orchestrator the exact pure normalizer call and provider context required in `src/openai.ts`.

## Integration order

1. The orchestrator creates the canonical lane from the exact base SHA after collision checks.
2. The orchestrator creates all three module lanes from the same exact base SHA. They may execute concurrently because their owned files are disjoint.
3. Integrate m03 first, then m02, then m01 with normal non-fast-forward merges.
4. Prove each accepted worker tip is an ancestor of the canonical tip with `git merge-base --is-ancestor`.
5. The orchestrator applies the one shared integration change in `src/openai.ts` and any shared response telemetry types. No worker owns this step.
6. Run combined focused tests, type checking, lint, formatting checks, build, whitespace checks, and the full repository test suite on the canonical lane.
7. Use one writer only for any combined-failure repair and final validation.
8. Stop at a clean, locally validated canonical commit unless the user gives current approval for push, deployment, and live probes.

## Validation matrix

### Static and focused validation

- `deno check` for every changed source entrypoint or the repository's canonical check task.
- Focused Codex adapter and account-routing tests.
- Focused prompt-cache analytics, handler, admin API, and admin UI tests.
- Focused Surplus and Metered response/usage tests.
- OpenAI Chat Completions and Responses compatibility tests for official cache fields.
- `deno fmt --check` on changed files only during implementation; use the repository final format gate before commit.
- Lint, build, and `git diff --check`.

### Combined behavior

- Identical keyed requests produce the same native Codex session identity and account preference.
- An eligible preferred account is selected without bypassing existing capacity rules.
- An authoritative quota result moves to the next allowed account or paid tier exactly as before.
- A timeout, network error, stall, read error, or upstream 5xx does not become quota exhaustion.
- Paid requests retain official cache controls and never receive Codex-private headers.
- Chat and Responses usage expose official cached and cache-write fields when upstream reports them.
- Analytics groups the same synthetic traffic correctly across every allowed dimension.
- Missing usage, unavailable KV, and legacy data are not displayed as real zeroes.

### Optional live acceptance after explicit approval

1. Record pre-deploy revision IDs and deploy one exact canonical commit.
2. Identify exactly one new succeeded revision, verify its immutable health identity, promote it by revision ID, and require HTTP 204.
3. Verify both managed Deno and custom-domain health identities. Treat a known Cloudflare challenge on the custom domain according to project guidance.
4. Run bounded sequential Luna Chat and Responses pairs with identical serialized bodies, stable explicit keys, and the least-privilege gateway token.
5. Require `x-uos-upstream: chatgpt_codex`, identical release tuples within each pair, matching account affinity where exposed internally, and nonzero cached tokens in at least one repeated cycle before claiming cache acceptance.
6. Confirm the admin analytics cohort records the live provider, model, route, keyed state, token hit, and request hit.
7. Run paid-provider pairs only under a separately approved target and spend cap. A cold result is evidence, not an implementation failure; repeat only within the approved request cap.

## Risks and required decisions

- Account affinity can concentrate traffic. Eligibility and quota checks must remain authoritative on every request.
- An authenticated scope may be broader than one person. Do not create cross-user affinity unless a stable per-credential principal is proved.
- A paid marketplace may route successive requests to different sellers internally. The gateway cannot correct that without a documented upstream affinity contract.
- A key is a routing hint, not a cache handle. Exact prefix matching still controls reuse.
- OpenAI guidance recommends keeping each prefix and key combination near 15 requests per minute. Do not introduce one global key.
- Model/provider wire behavior is time-sensitive. Do not convert an old pricing field into a terminal usage assumption.
- Responses continuation could improve reasoning cache use, but provider-native response IDs cannot safely cross failover. It remains out of scope.

## Completion and final report

The final report must state:

- canonical branch, worktree, base SHA, and final SHA;
- every task-created branch and worktree with disposition `integrated`, `rejected:<reason>`, or `blocked:<owner-and-next-action>`;
- ancestry proof for every accepted worker tip;
- exact changed files and behavior;
- focused and full validation results;
- whether paid native usage normalization was evidence-backed or intentionally unchanged;
- local, pushed, deployed, promoted, and live-accepted status as separate facts;
- production Git SHA and Deno revision only if deployment was authorized and verified;
- any remaining provider limitation or approval-gated probe.

Do not claim completion while accepted work is outside the canonical branch, the canonical tree is dirty, or a required validation result is missing.

## Paste-ready successor goal

Goal: Use canonical worktree name prompt-cache-affinity-observability-hand-g180c185110 at /Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-affinity-observability-hand-g180c185110 on branch codex/prompt-cache-affinity-observability-hand-g180c185110; read /Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md and /Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/prompt-cache-analytics-integration/docs/prompt-cache-affinity-observability-handoff-2026-08-22.md in full, then act as orchestrator and implement the plan end to end, delegate each write module only to its recorded isolated worktree, keep integration and final validation in the canonical worktree, preserve the fixed provider waterfall and approval boundaries, and never switch the canonical branch or worktree.
