# Codex Responses stream-drop diagnostic handoff — 2026-08-20

## Status

Diagnosis recorded. No repository source files, deployment configuration, branch, process, or production state were
changed while preparing this handoff. The incident remains unresolved at the client-visible compatibility layer.

## Objective and success criteria

Determine whether the production `stream closed before response.completed` failures are caused by the gateway or by a
Codex upstream stream ending after HTTP headers, and leave a safe, reproducible repair path for the next session.

Success requires all of the following:

- identify the upstream failure kind and whether `response.created` was observed;
- emit a Codex-recognized terminal event after semantic commitment, including when `response.created` was absent;
- preserve one terminal event and truthful failure telemetry;
- prove the behavior through the Responses handler and the terminal-log wrapper;
- if deployed, verify the client-visible SSE bytes and deployed revision against the approved SHA.

## Authoritative checkout and deployment state

- Repository: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Branch: `development`
- Local `HEAD`: `2bf836dfd22cea219e224fc4226ef1eb80c6ab43`
- `origin/development`: same SHA at the last check
- Production SHA: `2bf836dfd22cea219e224fc4226ef1eb80c6ab43`
- Production Deno deployment revision: `nqz9twaef18e`
- Last health snapshot (about 17:19 UTC on 2026-08-20): `/health` returned 200 and reported the matching SHA/revision;
  `/health/upstream` returned 200, with Codex and Metered probes returning 200.
- Authenticated Codex KV snapshot: one configured account, healthy and unexpired.
- Passive provider state: Metered was stale/degraded from earlier provider errors; Surplus was unknown. These are health
  snapshots, not inference proof.

### Dirty state to preserve

- `deno.json` is modified only by trailing blank-line noise.
- `docs/surplus-crypto-payments.md` is untracked.
- `docs/surplusintelligence.md` is untracked.

Do not reset, clean, overwrite, or stage these files as part of the incident work.

Working-tree update observed during the same-day follow-up (after this handoff was written): `src/handler.ts`,
`src/openai.ts`, and `src/responses_failover_stream.ts` now carry uncommitted changes that correspond to authorized
next-work items 1 and 3 — `failure_kind`, `response_created_observed`, and `synthetic_terminal_type` telemetry in
`request_terminal`, and a `syntheticFailure()` that keys on semantic commitment (any semantic event observed) instead of
`response.created` (`src/responses_failover_stream.ts`), so the no-`response.created` branch now emits the
Codex-recognized `response.failed` rather than the flat `error` event. These changes are uncommitted and were not
re-verified here; preserve them and re-run the focused and compatibility suites before any deployment.

## Production evidence

Captured terminal logs are at `/tmp/aiubq-deploy-logs-20260820.ndjson` with SHA-256
`995dfc161a555dbc5c23ad6ea2235ce4b01352f7c9e9d6a7f7113b5001406cc1`.

The capture contains 494 `request_terminal` records:

| HTTP/status and terminal type | Count |
| ----------------------------- | ----: |
| `200 / response.completed`    |   330 |
| `200 / error`                 |    38 |
| `200 / cancelled`             |     4 |
| `504 / deadline`              |     5 |
| `503 / error`                 |   117 |

All 38 relevant `200 / error` requests have these common fields:

- provider `chatgpt_codex`;
- model `gpt-5.6-luna`;
- account slot `1`;
- one Codex dispatch and HTTP 200 upstream headers;
- a first SSE event was observed;
- no clean terminal EOF/completion was observed.

The first SSE event occurred 1,485–11,116 ms after dispatch. The gateway terminal event followed 1–4,616 ms later; 28 of
38 failures ended within 10 ms of the first SSE event. This timing points to a post-header upstream reader/network
failure or malformed subsequent SSE event, not an authentication or pre-dispatch failure.

No authenticated live inference probe was run in this diagnostic session. Do not treat the health endpoints as a
substitute for the incident-shaped request.

Health snapshots are structurally unable to prove or refute this fault: Codex health observations are written **at
header time only** — `recordCodexResponseHealth` maps 2xx to `success`, 401/expired-token 403 to `auth_invalid`, 429 to
`quota_exhausted`, 5xx to `upstream_error` ([`src/codex.ts`](../src/codex.ts#L1353-L1372)), and
`recordCodexThrownHealth` covers thrown transport errors ([`src/codex.ts`](../src/codex.ts#L1374-L1392)); a mid-stream
drop after headers records no observation. Snapshots are per authenticated account in Deno KV
(`uos_ai/provider_health/v1/codex/{accountId}/…`, [`src/provider_health.ts`](../src/provider_health.ts#L4-L60),
30-minute staleness) and `/health/upstream` probes prove header-phase reachability only. The 2026-08-20 snapshot ("Codex
and Metered probes returning 200", one healthy authenticated account) is therefore consistent with — and says nothing
against — the 38 stream drops.

## Root-cause split

### Likely initiating fault: Codex upstream stream drop

The upstream Codex request reaches HTTP 200 and emits an initial semantic event, then the stream fails before a normal
terminal event. The available evidence cannot distinguish a transport reader error from a malformed later SSE event
without the new telemetry or a captured wire stream.

### Confirmed gateway compatibility defect: wrong terminal framing after commitment

The gateway's owned stream records `response.created` only when that event is visible
([`src/responses_failover_stream.ts`](../src/responses_failover_stream.ts#L411-L499)). On EOF or a reader exception, its
synthetic terminal selection is:

- with `response.created`: `response.failed` with a generated or known response ID
  ([`src/responses_failover_stream.ts`](../src/responses_failover_stream.ts#L331-L354),
  [`src/responses_failover_stream.ts`](../src/responses_failover_stream.ts#L575-L583));
- without `response.created`: a flat `error` event
  ([`src/responses_failover_stream.ts`](../src/responses_failover_stream.ts#L356-L366)).

The gateway treats `error` as a Responses terminal event
([`src/responses_stream.ts`](../src/responses_stream.ts#L4-L9)). The Codex client parser handles `response.failed` and
`response.incomplete`, but has no matching terminal branch for the flat `error` event
([`lib/codex/codex-rs/codex-api/src/sse/responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L387-L433)).
When that stream then closes, the client falls back to `stream closed before response.completed`
([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L509-L521)).

Verified client-parser mechanics (same-day follow-up analysis):

- `process_responses_event` ([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L327-L473)) matches
  only `response.output_item.done`, `response.output_text.delta`, `response.custom_tool_call_input.delta`,
  `response.reasoning_summary_text.delta`/`.done`, `response.reasoning_text.delta`, `response.created`,
  `response.failed`, `response.incomplete`, `response.completed`, `response.output_item.added`, and
  `response.reasoning_summary_part.added`. There is **no `"error"` arm**; the `_ =>` fallback traces "unhandled
  responses event" and returns `Ok(None)`
  ([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L467-L469)).
- The error-shape handling at [`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L387-L417) is
  **not** an `error`-event branch: it is the `response.failed` arm parsing the nested `response.error` object.
- `ResponsesStreamEvent` ([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L160-L175)) does not use
  `deny_unknown_fields`, so the gateway's flat payload (`type: "error"`, top-level `code`/`message`/`param`, plus
  `sequence_number`) deserializes successfully and reaches the unhandled arm rather than failing parse.
- The SSE loop consumes the result as `Ok(None) => {}` (event silently skipped) and assigns `response_error` only on
  `Err` ([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L581-L595)). A flat `error` event never
  produces `Err`, so at clean EOF `response_error` is still `None` and the client sends
  `ApiError::Stream("stream closed before response.completed")`
  ([`responses.rs`](../lib/codex/codex-rs/codex-api/src/sse/responses.rs#L516-L521)), which breaks the turn with that
  exact message ([`core/src/session/turn.rs`](../lib/codex/codex-rs/core/src/session/turn.rs#L2127-L2131)).
- The telemetry signature matches: the 38 incident rows are `200 / error` — the gateway's flat-error synthetic terminal
  recorded via `recordResponsesTerminal` as `stream_terminal_type: "error"`
  ([`src/openai.ts`](../src/openai.ts#L5044-L5053)). The sibling branch (with `response.created` observed) would surface
  as `200 / response.failed`, which the client recognizes and reports as "response.failed event received" — a different
  client message.

Hermetic reproduction already established the exact split:

1. semantic output delta, abrupt reader error, and no `response.created` -> HTTP 200, one `event: error`, telemetry
   failure, then the exact Codex client symptom;
2. the same reader failure after `response.created` -> `response.failed`, which Codex recognizes.

Therefore the upstream drop is the initiating fault, while the missing `response.failed` framing is the compatibility
defect that explains the user-visible message.

## Deployment comparison

Compared with the prior base `0e17c6f`, these files are unchanged at production SHA `2bf836d`:

- `src/responses_stream.ts`
- `src/responses_failover_stream.ts`
- `src/codex.ts`

The later deployment changes are catalog, routing, provider-health, paid-fallback, lifecycle, and Surplus catalog work
(`src/openai.ts`, `src/handler.ts`, and related files). The paid-fallback path does not replace a committed primary
Codex stream after HTTP 200. This is not strong evidence of a Surplus deployment regression; it is an upstream stream
failure exposed by a pre-existing gateway/Codex framing mismatch.

Independent deployment comparison (same-day follow-up, base `eec6b54` → production SHA `2bf836d`, 19 commits) confirms
the same conclusion: `src/responses_stream.ts` has zero changes in the range; `src/responses_failover_stream.ts` changes
only the OpenRouter→RemovedProvider strings (`7e0d791`); the `src/openai.ts` changes are renames, the surplus/metered
paid-fallback rework inside `fetchResponsesWithPaidFallback` (including the `meteredOnlyPrimary` pre-commit 429 gate),
and catalog aggregation. None touch downstream terminal emission.

The synthetic-terminal logic also predates both bases: it landed in `1405109` ("feat: add OpenRouter auto responses
failover with commit gate and circuit", 2026-08-13) and was refined in `29cd68b` and `2e21bac`; all three are ancestors
of `eec6b54` and `2bf836d`. The pre-`1405109` passthrough (`proxyResponsesStreamIterator` at `src/openai.ts:6600` in
`1405109^`) likewise emitted a flat `error` event on upstream read failure unless aborted
([`src/responses_stream.ts`](../src/responses_stream.ts#L340-L348)), so the flat-error framing gap is older than the
commit-gate work and cannot be a regression of either deployment window.

## Validation already completed

Recorded in the diagnostic run (source unchanged afterward):

- Responses stream and failover focused tests: 30 passed.
- `tests/openai-compat.test.ts`: 80 tests and 174 steps passed.
- Hermetic end-to-end reproduction of both terminal branches passed.
- No authenticated live inference acceptance was performed.

## Authorized next work

The current request authorizes this handoff only. Do not implement or deploy until the user explicitly authorizes the
repair.

1. Add safe, non-content telemetry for `failure_kind` (`premature_eof`, `read_error`, `malformed_event`,
   `inactivity_timeout`, or `event_too_large`), `response_created_observed`, and the synthetic terminal type. Keep
   prompts, tokens, response text, and raw upstream bodies out of logs.
2. Use one approved live request or one captured wire stream to confirm whether the failure is a reader error or
   malformed later SSE event. Preserve the evidence and avoid repeated live retries.
3. After semantic commitment, always synthesize `response.failed` with the known or generated response ID, including the
   no-`response.created` branch. Keep the pre-commit/no-semantic-output behavior separate and truthful.
4. Add a regression for semantic output without `response.created` followed by a reader error through
   `createOwnedResponsesStream`, `handleResponses`, and the `request_terminal` wrapper; assert one Codex-recognized
   terminal and no duplicate terminal.
5. Run focused tests, then the relevant OpenAI compatibility suite. Deployment requires separate approval; after
   deployment, verify the exact SSE terminal bytes, `/health` SHA, deployment revision, and one real Codex client
   result.

## Non-goals and safety boundaries

- Do not change the Codex client vendored source as the first repair boundary.
- Do not add a gateway-only wire alias, `[DONE]` sentinel, invented completion, or silent success on EOF.
- Do not infer upstream health from `/health/upstream` alone.
- Do not deploy, push, rotate credentials, alter KV/provider state, or consume shared inference quota without explicit
  approval.
- Do not clean the three dirty paths listed above.

## Successor continuation sentence

Read `/Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md` and
`/Users/nv/repos/ubiquity/ai.ubq.fi/docs/codex-responses-stream-drop-diagnostic-handoff-2026-08-20.md` in full, recheck
the recorded branch, dirty state, production SHA, and deployment revision, then continue the diagnostic and
implementation only after explicit authorization, preserving the evidence and proving a Codex-recognized terminal event
end to end.

## Required final report

Report the exact changed files and SHA, focused and compatibility-test results, telemetry branch observed,
client-visible terminal bytes, live/deployed SHA and revision, any remaining provider uncertainty, and the preserved Git
dirty state. State explicitly whether production deployment was performed.

## Implementation goal (ready to hand to the repair agent)

Copy the block below verbatim as the objective for a fresh agent. It is self-contained; the agent must read the
referenced files before touching anything.

---

**Objective:** Complete and verify the repair for the production `stream closed before response.completed` failures on
`/v1/responses` described in `docs/codex-responses-stream-drop-diagnostic-handoff-2026-08-20.md` (read it in full first,
plus `AGENTS.md`), so that every post-commit stream delivers exactly one Codex-CLI-recognized terminal event — including
after an upstream EOF/read failure when `response.created` was absent but semantic output was observed — with truthful
failure telemetry, and no client-visible "stream closed before response.completed".

**Starting state (verify before editing):** repository `/Users/nv/repos/ubiquity/ai.ubq.fi`, branch `development`,
production SHA `2bf836dfd22cea219e224fc4226ef1eb80c6ab43` (Deploy revision `nqz9twaef18e`). The working tree already
contains an uncommitted partial implementation in `src/handler.ts`, `src/openai.ts`, and
`src/responses_failover_stream.ts` (telemetry fields `failure_kind`, `response_created_observed`,
`synthetic_terminal_type`; `syntheticFailure()` keyed on `semanticCommitmentObserved`). Preserve it; complete it rather
than redoing it. Also preserve the pre-existing dirty paths: `deno.json` (trailing blank-line noise only),
`docs/surplus-crypto-payments.md`, `docs/surplusintelligence.md`.

**Success criteria (all must hold):**

1. Post-commit upstream EOF/read failure with semantic output observed emits a Codex-recognized `response.failed` with
   the known or generated response ID — including when `response.created` was absent. The flat `error` event remains
   only for the no-semantic-commitment (and pre-commit) cases, which must stay separate and truthful.
2. The client receives exactly one terminal event per stream; no duplicate terminals, no silent success, no `[DONE]`
   sentinel, no gateway-only wire aliases, and no changes to the vendored Codex client under `lib/codex`.
3. `request_terminal` records `failure_kind` (`premature_eof`, `read_error`, `malformed_event`, `inactivity_timeout`, or
   `event_too_large`), `response_created_observed`, and `synthetic_terminal_type`; logs never contain prompts, tokens,
   response text, or raw upstream bodies.
4. Regression tests prove both branches end-to-end (semantic output without `response.created` + reader error → one
   `response.failed`; same failure after `response.created` → one `response.failed`) through
   `createOwnedResponsesStream`, `handleResponses`, and the `request_terminal` wrapper.
5. Focused suites pass: responses stream/failover tests, `tests/openai-compat.test.ts`, plus `deno fmt --check`,
   `deno lint`, and type checking for the gateway (`deno task test` gate). Do not modify or run the pinned `lib/codex`
   submodule tests unless a gateway change requires them.

**Allowed work:** complete the in-flight telemetry and terminal-framing changes; use one approved live request or one
captured wire stream only if the telemetry cannot discriminate reader-error from malformed-later-SSE-event (avoid
repeated live retries); add/adjust tests; commit only the repair (never stage the pre-existing dirty paths).

**Forbidden without explicit user approval:** deployment, `deno deploy`, pushing, credential rotation, KV/provider-state
mutation, consuming shared inference quota, or changing production configuration. Do not treat `/health/upstream` as
inference proof.

**Deployment (only after approval):** deploy the exact approved SHA, then verify the client-visible SSE terminal bytes
with one real Codex client, `/health` reports the matching SHA, the deployment revision matches, and `request_terminal`
shows the expected `synthetic_terminal_type` / `failure_kind` for a reproduced drop.

**Final report (required):** exact changed files and commit SHA, focused and compatibility test results, telemetry
branch observed (`failure_kind` / `response_created_observed` / `synthetic_terminal_type`), client-visible terminal
bytes, live/deployed SHA and revision (or explicit "not deployed"), remaining provider uncertainty, and the preserved
Git dirty state.
