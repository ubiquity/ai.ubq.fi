# Local Codex review fixes handoff — 2026-08-21

## Status

Local review is complete. Implementation is pending. The current candidate is not ready to call good: six source defects
and one stale test expectation are confirmed, and the focused paid-fallback matrix fails in three places.

The user has approved:

- focused local test edits and runs;
- the existing `deno.json` addition of `"unstable": ["no-legacy-abort"]`;
- an isolated, local served HTTP/SSE proof.

This handoff does not authorize a commit, push, deployment, live provider request, shared-quota use, KV mutation,
external monitor change, process restart, or process termination.

## Role and objective

Act as the single implementation owner. Correct the confirmed local-review defects in the current uncommitted provider
reliability candidate. Keep provider outcome, client delivery outcome, and paid settlement truthful and separate.

Success means:

- every defect below has a focused regression assertion;
- the paid-fallback routing matrix passes with no failed step;
- true pre-commit cancellation returns the OpenAI-shaped 499 contract;
- a gateway timeout returns the OpenAI-shaped 504 contract even when the same request signal is aborted;
- a validated provider terminal cannot be overwritten by a later client-body cancellation;
- Metered and Surplus transport failures retain provider-specific error attribution;
- RemovedProvider telemetry does not retain a failed Codex account slot, cohort, or provider request ID;
- malformed array-valued Responses error payloads are rejected;
- the isolated local HTTP/SSE delivery proof passes under `no-legacy-abort`;
- no unrelated dirty work changes.

Do not expand this into a lifecycle refactor, provider redesign, deployment task, or production-hardening project.

## Canonical continuation state

- Repository and required worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Branch: `development`
- Local `HEAD`: `3cc9bb121f65a8c88861d7708323935577833e09`
- `origin/development`: `3cc9bb121f65a8c88861d7708323935577833e09`
- Handoff: `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/local-codex-review-fixes-handoff-2026-08-21.md`
- Tracked binary-diff SHA-256 before this handoff: `6040a1c63439565db415271f9f6ad1f7ffd97ad550a3ae2e72fc6be8c4515352`
- Review command: `codex review --uncommitted`, using Codex CLI `0.149.0`
- Active `codex review --uncommitted` processes at handoff: none

This is an existing dirty continuation. Do not create a clean branch or worktree: the reviewed behavior exists only in
this checkout's uncommitted files. Do not reset, stash, clean, copy over, commit, or move the changes to simplify the
task. Recheck `pwd`, branch, `HEAD`, status, and the tracked diff hash before editing. If they differ, inspect and
preserve the newer state; do not force the recorded snapshot back into place.

Before this handoff, the exact status was:

```text
 M deno.json
 M scripts/stage0-cache-telemetry-gate.ts
 M serve.ts
 M src/codex.ts
 M src/handler.ts
 M src/metered.ts
 M src/openai.ts
 M src/paid_fallback.ts
 M src/paid_fallback_ledger.ts
 M src/prompt_cache_scope_experiment.ts
 M src/prompt_cache_telemetry_gate.ts
 M src/provider_health.ts
 M src/responses_stream.ts
 M src/surplus.ts
 M tests/codex-auth-cache.test.ts
 M tests/health.test.ts
 M tests/kv-budget.test.ts
 M tests/openai-compat.test.ts
 M tests/paid-fallback-v3-cutover.test.ts
 M tests/prompt-cache-scope-experiment.test.ts
 M tests/responses-stream.test.ts
 M tests/stage0-cache-telemetry-gate.test.ts
?? docs/provider-reliability-efficiency-handoff-2026-08-21.md
?? docs/sub2api-internal-reliability-gap-audit-handoff-2026-08-21.md
?? tests/request-delivery-lifecycle.test.ts
?? tests/serve-delivery-http.test.ts
```

This document becomes one additional untracked file. The tracked diff hash does not cover any untracked file. Treat all
listed changes as user-owned. Read current diffs, not only files at `HEAD`.

## Required context

Read these files before editing:

- `/Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md`
- `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-reliability-efficiency-handoff-2026-08-21.md`
- this handoff in full;
- current diffs for `serve.ts`, `src/handler.ts`, `src/openai.ts`, `src/responses_stream.ts`, `src/paid_fallback.ts`,
  `src/paid_fallback_ledger.ts`, `src/provider_health.ts`, `src/metered.ts`, and `src/surplus.ts`;
- current diffs for the focused tests named in the validation section.

The separate sub2api audit handoff is not part of this implementation. Do not edit it or expand this task from it.

## Review evidence

`codex review --uncommitted` returned two actionable findings. It also violated the expected recursion guard and started
nested local reviews before all review processes exited normally. No process was signalled or terminated. Independent
read-only audits confirmed those findings and found the remaining source gaps and stale test below.

After review, this command was run from the canonical checkout:

```sh
deno test -A tests/openai-compat.test.ts --filter 'openai: Metered paid fallback routing matrix'
```

Result: the top-level test failed, with 3 failed steps and 85 filtered-out tests.

1. `cancellation before fallback admission creates no paid exposure` expected 502 but received 499.
2. `Metered network ambiguity returns an attributed 502 without retrying` expected `server_error` and
   `metered_upstream_unreachable` but received the generic Codex error contract.
3. `Metered pre-header deadlines return an attributed 504 without retrying` expected 504 but received 499.

Earlier focused checks passed for formatting, lint/check, local served delivery, provider health, the Responses parser,
cache telemetry, paid settlement, Responses HTTP, cancellation/deadline handling, inter-provider routing, KV logic,
streaming quota, and the Codex auth probe. Those earlier passes are useful baseline evidence, but they do not override
the failing matrix above.

## Confirmed findings

### 1. P1 — `TimeoutError` loses to generic cancellation

Evidence:

- `src/openai.ts:772-781`: `classifyPreHeaderFailure` checks `downstreamSignal.aborted` before the timeout reason.
- `src/openai.ts:8503-8511`: the outer Responses catch also checks the aborted signal before `TimeoutError`.
- `tests/openai-compat.test.ts:4159-4227`: the paid-fallback matrix requires 504, `server_error`, and `gateway_timeout`
  for all buffered and streaming Chat and Responses cases.

Required correction:

- Classify by the effective abort reason before treating an aborted downstream signal as client cancellation.
- `TimeoutError` or the gateway deadline signal must produce terminal type `deadline` and HTTP 504 before commitment.
- `AbortError` from a real client cancellation must remain terminal type `cancelled` and HTTP 499 before commitment.
- Apply the same rule to Chat and Responses without a broad rewrite.

### 2. P1 — a validated provider terminal can be overwritten by delivery cancellation

Evidence:

- `src/openai.ts:8127-8139` and `src/openai.ts:8287-8297` can validate and buffer a Responses terminal before the
  response is handed to the client.
- `src/openai.ts:8484-8486` always reconciles client-body cancellation as provider cancellation.
- `src/openai.ts:5957-5964` does the same in the Chat stream wrapper.

If `response.completed` is already buffered and the client then cancels without consuming it, current code can release a
successful Codex probe or reconcile a paid request as cancelled. The provider already completed and its usage can be
known, so that result is false.

Required correction:

- Give a validated upstream terminal precedence over later client delivery cancellation.
- Record provider terminal state exactly once.
- Record client delivery cancellation separately.
- Do not release, downgrade, or cancel a completed provider probe or paid settlement because the client did not consume
  an already-buffered terminal.
- Cover both `response.completed` and `response.incomplete` so a completion-only guard cannot pass. Cover both Responses
  and Chat, and cover Codex and paid fallback where the current test harness can do so without a live request.

### 3. P2 — paid-provider transport errors lose their provider-specific contract

Evidence:

- `src/openai.ts:2482-2487` records ambiguity and rethrows when Metered or Surplus transport started but failed before
  response headers.
- `src/openai.ts:1596-1624` recognizes Codex and API-key errors only, so the rethrown transport error becomes
  `codex_upstream_unreachable` with the wrong default error type.
- `tests/openai-compat.test.ts:4085-4153` requires one Metered attempt, HTTP 502, `x-uos-upstream: metered`,
  `server_error`, `metered_upstream_unreachable`, and pending ambiguous billing.

Required correction:

- Preserve the selected paid provider in the returned error envelope and header.
- Use the matching Metered or Surplus error code and `server_error` type.
- Keep the current no-retry rule after transport starts.
- Keep the ambiguous ledger record and pending billing state.
- Add the equivalent Surplus assertion if the existing matrix does not cover it.

### 4. P2 — true post-header cancellation can become a false 502 before HTTP commitment

Evidence:

- Chat preflight records `cancelled` at `src/openai.ts:7753-7762`, but `streamPreflightFailureResponse` at
  `src/openai.ts:804-825` handles only deadline and otherwise returns 502.
- buffered Chat records `cancelled` at `src/openai.ts:6064-6069`, then returns 502 at `src/openai.ts:6078-6086`.
- buffered Responses reconciles cancellation at `src/openai.ts:8448-8455`, while `collectBufferedResponses` returns 502
  at `src/openai.ts:1445-1453`.

Required correction:

- If cancellation is known while the gateway can still select the HTTP response, return 499 with error type
  `server_error`, code `request_cancelled`, and `param: null`.
- If HTTP 200 is already committed for an SSE response, keep HTTP 200 and record the interrupted delivery and terminal
  state without inventing a later HTTP error.
- Do not let this correction overwrite an upstream terminal that was already validated; finding 2 has precedence.

### 5. P2 — the Responses parser accepts `error: []`

Evidence:

- `src/responses_stream.ts:79-87` uses `isRecord(value.error)` without rejecting arrays.
- `tests/responses-stream.test.ts:99-113` rejects array-valued `response` payloads but does not cover an array-valued
  `error` payload.

Required correction:

- Reject arrays for the nested error object, as already done for terminal response objects.
- Preserve support for the official flat error terminal shape.
- Add a parser regression case for `data: {"type":"error","error":[]}`. Add a public route assertion only if it is cheap
  and stays within the focused test surface.

### 6. P2 — RemovedProvider fallback retains failed Codex provider metadata

Evidence:

- `src/openai.ts:2191-2194` sets the Codex account slot, cohort, and provider request ID.
- `src/openai.ts:2330-2334` clears both fields when paid fallback becomes selected.
- RemovedProvider selection changes only the provider at `src/openai.ts:1252-1253`, `src/openai.ts:8249`, and
  `src/openai.ts:8284`.
- `src/handler.ts:195-223` logs final provider, request ID, account slot, and account cohort together. The stale request
  ID can also reach the response header through the final telemetry path near `src/handler.ts:801`.

Required correction:

- Clear `accountSlot` and `accountCohortId` whenever RemovedProvider becomes the selected provider.
- Clear `providerRequestId`, or replace it only with a trustworthy request ID from RemovedProvider.
- Cover the Codex-to-RemovedProvider transition and the recovery path that can return RemovedProvider.
- Assert that both the response header and terminal log contain no failed Codex request ID after this transition.
- Keep provider-specific dimensions only when they describe the final selected provider.

### 7. Test correction — cancellation before paid admission already returns the correct 499

Evidence:

- `tests/openai-compat.test.ts:3727-3785` aborts with `AbortError` before fallback admission.
- The test expects 502 at line 3773, but the current response is the correct 499.
- The same test already proves one Codex call, zero Metered calls, no paid request record, and no paid window.

Required correction:

- Change the expected status to 499.
- Assert the OpenAI-shaped `request_cancelled` error and `server_error` type.
- Assert terminal type `cancelled` when available.
- Preserve every no-paid-exposure assertion.

## Behavioral contracts

Use these contracts to resolve interactions between the findings:

| Situation                                                     | HTTP result before commitment   | Provider terminal                                                            | Delivery outcome                                                                        | Billing and health                                                  |
| ------------------------------------------------------------- | ------------------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Real client `AbortError` before any provider terminal         | 499 `request_cancelled`         | cancelled or not started                                                     | `interrupted` for a disconnect; otherwise use `ServerInfo.completed`                    | no invented success; no paid exposure before admission              |
| Gateway `TimeoutError` before commitment                      | 504 `gateway_timeout`           | deadline or ambiguous as the existing ledger contract requires               | `delivered` if the 504 body reaches the client; otherwise `interrupted` or `unobserved` | preserve provider attribution and pending ambiguity after transport |
| Provider terminal already validated, then client cancels body | existing response, normally 200 | preserve validated terminal                                                  | `interrupted`                                                                           | settle and score from provider terminal, not delivery interruption  |
| SSE response already committed, then client cancels           | keep 200                        | preserve a prior terminal; otherwise cancelled/ambiguous by current contract | `interrupted`                                                                           | never invent a second terminal or settlement                        |
| Paid transport began, then failed before headers              | 502 with selected provider code | ambiguous                                                                    | `delivered` if the 502 body reaches the client; otherwise `interrupted` or `unobserved` | no retry; pending ambiguous billing                                 |

Additional invariants:

- Provider outcome, response delivery, client consumption, and paid settlement are separate state dimensions.
- Delivery outcome uses only `delivered`, `interrupted`, or `unobserved`, derived from `ServerInfo.completed`; a
  provider deadline or error does not itself mean delivery failed.
- Emit at most one provider terminal transition and perform at most one paid settlement transition.
- Never report provider health success from HTTP delivery alone.
- Never report client cancellation only because a gateway deadline aborted a shared signal.
- The final provider owns provider-specific telemetry. RemovedProvider and paid providers have no Codex account slot,
  cohort, or request ID. A provider request ID is present only when it belongs to the selected provider.
- Keep official OpenAI-compatible request and error shapes. Do not add a gateway-only wire field.
- Preserve `"unstable": ["no-legacy-abort"]` in `deno.json`; do not add a new CLI flag or environment variable.

## Implementation order

Keep one writer because `src/openai.ts`, lifecycle telemetry, provider health, and settlement are shared surfaces.
Read-only review can run later, but do not split source edits across parallel workers.

1. Recheck canonical state and read every current diff in scope.
2. Make the smallest source corrections for abort-cause classification and pre-commit 499/504 response selection.
3. Add an explicit, local exactly-once guard or equivalent state check so a validated provider terminal wins over a
   later body cancellation. Keep delivery recording separate.
4. Preserve provider-specific paid transport errors while retaining the no-retry and ambiguous-settlement rules.
5. Reject array-valued nested Responses errors and clear Codex account metadata on RemovedProvider selection.
6. At the end of source work, update the stale expectation and add only the focused regression cases required above.
7. Run the focused validation matrix. Fix only defects exposed on this owned surface.
8. Run the isolated served HTTP/SSE proof.
9. Do not run another `codex review --uncommitted` without fresh user approval. The approved review already completed,
   and it recursively spawned local review processes. If the user approves one more run and it recurses again, do not
   retry or signal any process; let user-owned review processes finish and report the review as locally unavailable.
10. Stop and report. Do not deploy, push, commit, or issue a live provider request.

## Focused validation matrix

Run from `/Users/nv/repos/ubiquity/ai.ubq.fi`.

### Required focused behavior

```sh
deno test -A tests/openai-compat.test.ts --filter 'openai: Metered paid fallback routing matrix'
deno test -A tests/responses-stream.test.ts
deno test -A tests/request-delivery-lifecycle.test.ts
deno test -A tests/serve-delivery-http.test.ts
deno test -A tests/paid-fallback-v3-cutover.test.ts
```

Add a narrower filter instead of another broad suite if a new regression case lives in `tests/openai-compat.test.ts`
outside the existing matrix. Do not run `deno task test` as part of this focused handoff unless the user gives broader
test approval.

### Static checks

After behavior passes, run checks only over files changed for these corrections and their focused tests:

```sh
deno fmt --check src/openai.ts src/responses_stream.ts tests/openai-compat.test.ts tests/responses-stream.test.ts tests/request-delivery-lifecycle.test.ts tests/serve-delivery-http.test.ts
deno lint src/openai.ts src/responses_stream.ts tests/openai-compat.test.ts tests/responses-stream.test.ts tests/request-delivery-lifecycle.test.ts tests/serve-delivery-http.test.ts
deno task build
git diff --check
```

If another owned file must change, include it in formatting and lint. Do not mechanically format unrelated dirty files.

### Local served proof

`tests/serve-delivery-http.test.ts` must use an isolated loopback server and prove real HTTP/SSE body delivery and
consumer cancellation under the configured Deno abort semantics. It is local proof only. It is not a real provider,
deployed revision, or production proof.

## Acceptance requirements

Do not report the changes as good until all of these are true:

- all three currently failing matrix steps pass;
- Chat and Responses distinguish `AbortError` from `TimeoutError` for streaming and buffered modes;
- true cancellation returns 499 before commitment and never opens paid exposure before admission;
- buffered `response.completed` and `response.incomplete` terminals survive later client-body cancellation;
- Codex probe state, paid ledger state, provider health, usage, and terminal telemetry each change at most once;
- Metered and Surplus transport failures keep the selected provider header, code, and `server_error` type;
- `error: []` fails parsing while the official flat error event still passes;
- RemovedProvider terminal telemetry has null account slot and cohort, and no failed Codex request ID in its response
  header or terminal log;
- focused tests, static checks, `git diff --check`, and the isolated served proof pass;
- the final tracked and untracked status is reported, with unrelated changes preserved;
- the final report distinguishes local source, test, and served evidence from live or production evidence;
- every existing local Codex review finding is resolved; no post-fix review is required without fresh user approval.

## Safety boundaries and non-goals

- Do not use real Metered, Surplus, Codex, Cerebras, or other provider quota.
- Do not deploy or inspect production as a substitute for the approved local proof.
- Do not mutate Deno Deploy state, KV, credentials, balances, external monitors, Git remotes, branches, or worktrees.
- Do not restart, stop, detach, or signal Codex, Deno, browser, tmux, SSH, or other user-owned processes.
- Do not add an environment variable, secret, command-line argument, or new CLI flag.
- Do not remove `no-legacy-abort` or add a compatibility fallback to legacy abort behavior.
- Do not change OpenAI-compatible endpoint schemas or Codex CLI compatibility contracts beyond the confirmed error and
  lifecycle corrections.
- Do not refactor unrelated routing, cache measurement, telemetry, health, or settlement code.
- Do not edit the two existing untracked handoffs except on a separate explicit request.

## Required final report

Report:

- each finding and its final disposition;
- changed files;
- exact focused commands and pass/fail counts;
- local served HTTP/SSE evidence;
- the existing local Codex review result and whether a post-fix review was separately authorized;
- final branch, `HEAD`, tracked diff hash, and full dirty status;
- whether any commit, push, deployment, live request, shared quota, KV mutation, external change, or process action
  occurred;
- remaining uncertainty, especially the lack of live paid-provider and deployment proof.

## Successor continuation sentence

Read /Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md and
/Users/nv/repos/ubiquity/ai.ubq.fi/docs/local-codex-review-fixes-handoff-2026-08-21.md in full, act as the single
implementation owner in the existing dirty /Users/nv/repos/ubiquity/ai.ubq.fi development checkout, correct all
confirmed review findings with the smallest local changes, run only the approved focused and isolated served validation,
and do not commit, push, deploy, consume provider quota, mutate external state, or signal user-owned processes.
