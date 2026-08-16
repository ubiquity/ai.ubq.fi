# Integration triage: `4bfc3b0...00bb8ac`

Source comparison: <https://github.com/ubiquity/ai.ubq.fi/compare/4bfc3b05649b900d88a45391fff0ea1dae45a2a2...00bb8ac6402acfebfd5b2d0e03b240e9154b6ad7>

## Decision

Do **not** merge or cherry-pick this comparison as a range. It is a 363-commit
integration history with merge/consolidation commits, checkpoint commits,
duplicated follow-up fixes, a revert, and a `wip(marketplace)` commit. It changes
74 files (+18,473/-1,123 lines). The correct approach is to select a small,
dependency-closed series, test it on a temporary integration branch, and then
repeat for the next capability.

This document records what the target claims to add or fix. “Implemented” means
the final target contains code and focused tests for the behavior; it does not
mean this review performed a live deployment or production acceptance test.

## What the range contains

| Area | Claimed behavior in the target | Evidence in the comparison | Triage |
| --- | --- | --- | --- |
| Codex account routing and timeouts | Shares requests across configured Codex accounts, tracks account-level quota/error state, and fences response-header timeouts so they count as server failures rather than quota exhaustion. | PR #77 merge `d29481c`; implementation commits `680f728`, `c7f905f`, and `d57947a`; 6 files, +922/-62 including 680 focused test lines. | **First candidate**, but still a substantial patch series. |
| Quota-class recovery | Keeps separately metered model classes from contaminating one another’s quota/circuit state, including legacy/unknown fence migration. | `ab52417` through `5760fc3`, principally `src/codex_account_routing.ts` and its focused tests. | **Second candidate** only after account routing is accepted. |
| OpenRouter automatic Responses failover | On a qualifying Codex outage, projects an eligible Responses request to OpenRouter, buffers/commits semantic output, maintains a circuit, and attempts to preserve SSE terminal semantics, tool loops, deadlines, and replay state. | Initial `1405109` alone touches 27 files (+4,237/-218); many later fixes through `8addd0a`, `04e5cbd`, and `00bb8ac`. | **Do not cherry-pick now.** Reimplement or import only after an isolated failure-mode acceptance plan. |
| Cerebras GPT-OSS Responses bridge | Routes the supported GPT-OSS path through Cerebras while translating the Responses request/stream contract and catalog. | `aa7c22c` initially changes 7 files (+932/-31); later compatibility corrections span `96f19a5` through `7357d80`. | **Defer.** It is a product feature with a long correction tail, not a small fix. |
| Autonomous-agent discovery | Serves `llms.txt` and `openapi.json`, documents the agent integration contract, and adds static-asset tests. | `a59b347` plus required bundling fix `6ffe6a0`: `src/static.ts`, `static/llms.txt`, `static/openapi.json`, docs, and tests. | **Low-risk later candidate**; review the generated OpenAPI contract before pick. |
| Marketplace auth accounts | Adds marketplace account CRUD/disable/current-user routes and provider capacity/account data. | Starts as `f99cd11 wip(marketplace)`; subsequent security/validation fixes include `1e08802` and `16458cc`. | **Exclude.** The range itself labels the foundation WIP and this introduces a separate product/security surface. |
| Admin and capacity UI | Adds/changes capacity telemetry, account labels, outage bands, historical chart scrolling, a forced Codex-503 debug switch, and cache-busted admin assets. | Several overlapping August commits, including `a0d63b4`, `fa380cf`, `7c3ba47`, `a8e3351`, `a2128f1`, and `09643b5`. | **Defer.** This is noisy UI/debug work; select only after a user-visible need is defined. |
| Prompt-cache telemetry and banked resets | Preserves cache telemetry/capability metadata, removes API-key IDs from logs, adds a telemetry gate, and adds a guarded/status-only Codex banked-reset workflow. | `fdfcb38` through `ccfd2aa`; `a695373`, `65b2457`; then multiple consolidation merges. | **Audit separately.** It combines spending/quota policy with diagnostics and must not enter incidentally. |
| Deployment and preview auth | Restores preview deployment automation and changes preview secret/bootstrap and passkey relay/CORS behavior. | `3f6c846` through `190e7c6`, PR #79, and PRs #81–84; includes a revert (`4511f29`). | **Exclude from application integration.** Deployment/auth changes need their own live preview validation and explicit authority. |

## Recommended first integration: timeout circuit

Start with the three non-merge commits below, in order, on a fresh branch from
`4bfc3b0`. Do not cherry-pick merge `d29481c`; it only records the PR merge.

1. `680f728` — fence upstream response-header timeouts.
2. `c7f905f` — address the circuit review findings.
3. `d57947a` — preserve timeout-probe classification.

This is the best first target because its stated purpose is operationally
specific, it has a bounded six-file final diff, and its tests cover the affected
routing, auth-cache, and OpenAI-compatibility paths. Before accepting it, run
the relevant focused tests and an isolated two-account request simulation that
proves all of the following:

- a header timeout does not consume or poison quota state;
- only the affected account enters its timeout circuit state;
- a later probe can recover the account;
- a normal upstream `429` remains distinct from a timeout;
- the `/v1/responses` and `/v1/chat/completions` client contracts remain intact.

The test surface matters here: source diff or unit tests alone are insufficient
proof for shared-account routing.

## Explicit exclusions

Do not pick these commits or commit classes as part of a feature selection:

- `merge: consolidate ...`, `Merge pull request ...`, `Merge origin/...`, and
  `chore(git): record ... ancestry` commits. They describe graph reconciliation,
  not a reviewable feature boundary.
- `chore: checkpoint ...` commits. They are workstation-recovery snapshots.
- `f99cd11` and its marketplace descendants until marketplace is an approved,
  separately tested project.
- Preview deployment, secret, passkey relay, and CORS changes without an
  authorized preview rollout and browser acceptance check.
- The full OpenRouter failover sequence. It has at least one feature commit,
  many semantic-stream corrections, duplicate-looking follow-ups, and a final
  fallback-model adjustment. Picking an arbitrary subset is likely to produce
  an unsafe or internally inconsistent stream state machine.

## Why the comparison looks sloppy

The concern is supported by the shape of the history, not merely the line
count:

- The comparison includes 363 commits but only covers five days after the base;
  much of it is imported branch ancestry rather than a coherent feature series.
- The two largest production changes are coupled to very large test rewrites:
  `src/openai.ts` (+1,712/-232) and `tests/openai-compat.test.ts` (+2,745/-280).
  That makes conflicts and accidental contract drift likely during blind picks.
- OpenRouter failover begins with a 27-file patch and then receives many fixes
  for commitment, terminal error, deadline, output-form, and replay edge cases.
  This is evidence of an unfinished or difficult-to-maintain boundary, even
  though the final target has tests for it.
- Marketplace begins from an explicitly named WIP checkpoint, then needs
  security and validation follow-ups. It should not piggyback on gateway
  reliability work.
- The target carries duplicated UI/asset/cache-busting changes and an explicit
  revert in preview passkey work. Those are integration-history artifacts, not
  reasons to expand a gateway patch.

`git diff --check 4bfc3b0 00bb8ac` reports no whitespace errors. That is useful
only for patch hygiene; it does not establish correctness, deployability, or
production safety.

## Sequencing after the first patch

1. Integrate and prove the timeout circuit as the only behavior change.
2. Decide whether the quota-class series solves an observed production problem;
   if yes, integrate it as its own series with mixed-model routing tests.
3. Pick the agent-discovery pair only if the published discovery documents are
   wanted, after schema review.
4. Treat Cerebras and OpenRouter as independent feature projects, each with a
   written provider contract, failure matrix, and real streaming acceptance
   check before any cherry-pick.

No changes from this report have been cherry-picked, merged, deployed, or
tested against a live service.
