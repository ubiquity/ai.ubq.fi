# Provider reliability and efficiency handoff — 2026-08-21

## Status

Production audit complete. No source, deployment, provider, credential, KV, process, or external monitoring state was
changed during the audit. This document authorizes no implementation, paid inference canary, deployment, or push.

## Objective

Improve the reliability, efficiency, and operational evidence of the `ai.ubq.fi` provider gateway without weakening its
OpenAI-compatible contracts or its Codex CLI compatibility.

Success means:

- normal Codex inference remains reliable and keeps truthful terminal telemetry;
- the gateway no longer relies on Deno's legacy successful-response abort behavior;
- paid fallbacks have current, incident-shaped evidence before an outage requires them;
- obsolete and unauthenticated probes stop creating errors, warnings, and unnecessary invocations;
- prompt-cache performance is measured and improved without logging request content;
- any deployed result is verified on the exact production SHA and Deno revision.

## Authoritative state at handoff

- Repository: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Branch: `development`
- Local `HEAD`: `eeb68ef61a7504bc9a78351d0ea6c8ef83a1cf88`
- Working tree before this document: clean
- Production URL: `https://ai.ubq.fi`
- Production SHA: `88ed1b854fd8f1571dd39b197f77bf6436d7d851`
- Production Deno revision: `bpw68mckq1r9`
- Production `/health`: HTTP 200 with `status: available` and the matching SHA and revision
- Log capture: `logs/deno-deploy-20260821T110122Z.jsonl`
- Capture window: approximately 2026-08-21 10:01–11:01 UTC
- Capture size: 1,018 records

The capture is local evidence and may be ignored by Git. Preserve it while this work is active. Recheck the checkout,
production identity, provider health, and log freshness before implementation or acceptance because all can drift.

This is a direct, single-writer continuation. It does not require a new branch, worktree, module lane, or subagent.
Request lifecycle, provider health, and telemetry are shared surfaces and should remain under one implementation owner.

## Confirmed production evidence

### Inference reliability

The capture contains 296 `request_terminal` records:

| Measure                                        | Observed value |
| ---------------------------------------------- | -------------: |
| HTTP 200 terminal requests                     |     296 of 296 |
| Terminal provider failures                     |              0 |
| Requests with more than one attempted provider |              0 |
| Retries or failovers observed                  |              0 |
| Median total latency                           |       8,508 ms |
| p95 total latency                              |      36,991 ms |
| Maximum total latency                          |     133,280 ms |
| Median provider dispatch                       |         612 ms |
| Median upstream headers                        |       1,544 ms |

All terminal requests used `chatgpt_codex`. The longest request produced 7,246 output tokens and received its first SSE
event after 6,609 ms. It therefore shows long generation, not a pre-header gateway stall. Do not reduce inference
deadlines based on total latency alone.

### Prompt-cache efficiency

| Measure                                  | Observed value |
| ---------------------------------------- | -------------: |
| Input tokens                             |     36,774,759 |
| Cached input tokens                      |     19,136,000 |
| Aggregate cache ratio                    |         52.04% |
| Requests with zero cached tokens         |             62 |
| Requests with more than 80% cached input |            120 |
| Uncached input tokens                    |     17,638,759 |

Of the 296 requests, 294 used `gpt-5.6-luna`. A prompt cache key was present on the Responses traffic, but cache results
varied sharply between adjacent minutes. The logs do not contain request content or cache-key values, so they cannot
prove whether misses came from changing prompt prefixes, changing tool order, changing keys, or upstream cache eviction.
Preserve this privacy boundary.

### Passive provider health

The authenticated passive `/health/providers` snapshot reported:

- Codex: configured, one KV account, healthy, current success, token unexpired;
- Metered: configured, quota data available and fresh, but provider health degraded and stale after an earlier 401 and
  later upstream error;
- Surplus: configured, no quota monitor, health unknown, and no recorded provider attempt;
- Cerebras: configured and last successful, but stale.

This is passive evidence only. It does not prove that Metered or Surplus can complete the incident-shaped request that
would need them. The captured hour exercised neither paid fallback.

### Deno request lifecycle warnings

The active revision emitted five warnings that `request.signal` aborts on successful responses under legacy behavior.
The gateway composes the request signal into upstream inference deadlines in `src/inference_deadline.ts`. Deno advises
using the handler information object's `completed` promise for delivery completion or opting into the new abort behavior
with `--unstable-no-legacy-abort`.

This is a correctness risk for cleanup, cancellation classification, and any accounting tied to an abort. It is not a
confirmed inference failure in this capture.

### Obsolete revision health errors

Revision `ts4w36evn1xc` started seven isolates during the capture and failed twice in its health path:

`TypeError: Cannot read properties of undefined (reading 'split')`

The stack ended at `getJwtExpMs` while processing health metadata. These errors did not come from the stable production
revision. The current source validates stored auth pools before use, but `getJwtExpMs` still assumes a string at its
function boundary. Determine which monitor or direct deployment URL still invokes the obsolete revision before changing
current code. Remove or redirect the obsolete caller when ownership is known.

### Unauthenticated model polling and isolate starts

- The gateway rejected 41 unauthenticated `/v1/models` requests, generally on a three-minute cadence.
- The active revision logged 70 isolate starts during the hour.

The first pattern strongly suggests a misconfigured liveness or catalog monitor. It should use public `/health` for
liveness or valid client authentication for `/v1/models`. The isolate count may reflect normal serverless scaling for
long streams; do not call it a defect without Deno usage and concurrency evidence.

## Required work

### 1. Correct request completion and cancellation handling

Inspect `serve.ts`, `src/handler.ts`, `src/inference_deadline.ts`, stream ownership, billing settlement, and terminal
telemetry before editing. Define these states explicitly:

- client disconnected before response delivery;
- handler returned a successful streaming response;
- response body was fully delivered;
- upstream or gateway deadline expired;
- upstream stream failed after semantic commitment.

Use Deno's current completion semantics so a successfully delivered response cannot be classified as a client abort. Do
not merely suppress the warning. Preserve real client-disconnect cancellation of upstream work. Make the smallest
coherent change and keep exactly one truthful terminal record and one settlement result per request.

### 2. Repair provider-health evidence

Keep `/health` passive and free of paid inference. Extend or correct passive observations only when a real provider
operation already supplies trustworthy evidence. In particular:

- explain Metered's stale degraded state and confirm whether the current key and endpoint are usable;
- keep Surplus `unknown` when no request has exercised it; do not convert catalog reachability into inference success;
- preserve `Quota: Not reported` or equivalent unknown state when a provider exposes no trustworthy balance;
- correlate provider request IDs when available without logging credentials or response bodies.

An active fallback canary is a separate operation. It must use the same endpoint family and request shape needed during
failover, record exact cost and provider route, and run only after the user approves shared quota consumption.

### 3. Remove probe waste and stale-revision noise

Identify the owner and target of the three-minute unauthenticated `/v1/models` polling and the direct traffic to
revision `ts4w36evn1xc`. Prefer changing the caller:

- liveness checks use `GET /health`;
- authenticated catalog checks use `GET /v1/models` with the existing client interface;
- production checks target the stable application URL and attest its SHA/revision;
- no monitor targets an obsolete immutable deployment unless it is an intentional historical probe.

Do not weaken `/v1/models` authentication and do not hide actionable auth failures globally to reduce log volume.

### 4. Measure and improve cache effectiveness

First add or use aggregate, content-free measurements that can distinguish stable cohorts by provider, model, route,
account slot, prompt-cache mode, and cache-key presence. Do not log the key value, prompts, tools, inputs, outputs, or
raw provider bodies.

Then inspect the client-to-upstream projection for avoidable prefix instability. Candidate causes include changing
system-prefix bytes, non-deterministic tool ordering, unstable metadata before the reusable prefix, and conversation
affinity changes. Preserve official request semantics. Do not reorder user-visible content or invent a gateway-only wire
field solely to improve caching.

Use a before-and-after window with comparable traffic. Report total input, cached input, cache ratio, zero-cache
requests, latency, model, route, and sample size. Do not claim savings from a synthetic benchmark alone.

### 5. Evaluate startup work only with cost evidence

Measure isolate count, request concurrency, stream duration, initialization latency, and billed Deno usage before
optimizing. If startup work is material, keep provider catalog loading, KV access, and other initialization lazy and
deduplicated per isolate. Do not introduce cross-request in-memory correctness dependencies; Deno Deploy is serverless.

## Implementation order

1. Refresh production logs and exact deployment identity without changing runtime state.
2. Trace `request.signal`, response delivery completion, terminal telemetry, and settlement through current source.
3. Implement and locally verify the smallest lifecycle correction.
4. Identify and correct external probe targets if the user authorizes changes to their owning systems.
5. Improve passive health observations without issuing paid inference.
6. Add privacy-safe cache-cohort measurement, then make only evidence-supported cache changes.
7. Ask for approval before any live inference canary, deployment, restart, push, or external monitor mutation.
8. If approved, deploy through the existing workflow and run the validation matrix below.

## Validation matrix

| Surface                 | Required evidence                                                                                                  |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------ |
| Static                  | `deno check` or the repository build task for changed source; lint and formatting only for owned files             |
| Focused lifecycle       | Successful stream delivery does not become an abort; real disconnect still cancels upstream work                   |
| Terminal accounting     | Exactly one terminal record and one truthful settlement for success, disconnect, deadline, and post-commit failure |
| Provider health         | Passive state remains truthful; unknown is not promoted to healthy without qualifying evidence                     |
| Cache                   | Comparable before/after production cohorts with content-free aggregate metrics                                     |
| Local runtime           | Real served HTTP and SSE behavior through `serve.ts`, not only direct function tests                               |
| Live, if approved       | Incident-shaped request reaches the intended provider and returns the expected terminal framing                    |
| Deployment, if approved | `/health` and headers report the exact approved Git SHA and Deno revision                                          |
| Post-deploy logs        | No new legacy-abort warnings or health exceptions; terminal outcome and route are present                          |

Do not run broad tests by default. Ask the user before adding or running tests. Focused tests are still required before
a deployment, but test authorization must be explicit in the implementation session.

## Safety boundaries and non-goals

- Do not deploy, push, restart or terminate a process, mutate KV, rotate credentials, edit external monitors, or consume
  provider quota without current user authorization.
- Do not log prompts, tool definitions, cache-key values, response content, raw authorization, or provider bodies.
- Do not weaken client authentication or OpenAI-compatible endpoint contracts.
- Do not fabricate quota, balance, provider success, or failover proof.
- Do not add an environment variable, secret, CLI flag, or command-line argument without asking first.
- Do not tune timeouts from total request latency; distinguish dispatch, headers, first event, inactivity, and complete
  generation.
- Do not optimize serverless isolate starts without billed-usage and concurrency evidence.
- Do not refactor unrelated provider routing, rename files, or introduce a generalized monitoring system.

## Decisions and blockers

- The active Codex path needs no emergency reliability repair based on this one-hour sample: it completed 296 of 296
  terminal requests.
- Paid fallback reliability remains unproved. A live canary is blocked on explicit approval because it consumes shared
  quota and creates an external provider action.
- External probe correction is blocked until the caller and owner are identified; repository changes must not guess at
  external ownership.
- Cache improvements require privacy-safe cohort evidence. Current logs identify the opportunity but not its cause.

## Completion requirements

Before reporting completion:

- recheck branch, worktree status, production SHA, Deno revision, and provider health;
- prove each changed behavior on its real acceptance surface;
- preserve unrelated dirty work;
- report every live request, its provider route, and whether it consumed quota;
- distinguish local, live, deployed, and production evidence;
- state whether any deployment, push, monitor mutation, KV mutation, or process action occurred;
- report remaining uncertainty for Metered, Surplus, cache causality, and Deno startup cost.

## Successor continuation sentence

Read `/Users/nv/repos/ubiquity/ai.ubq.fi/AGENTS.md` and
`/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-reliability-efficiency-handoff-2026-08-21.md` in full, act as the
single implementation owner on the existing `development` checkout, refresh the recorded production evidence, and
implement the provider reliability and efficiency work only within the document's approval and safety boundaries,
proving each accepted change on its stated real surface without deploying or consuming paid quota unless explicitly
authorized.

## Required final report

Report changed files and commits, final Git state, focused validation, local served evidence, live request routes and
costs, exact deployed SHA/revision if applicable, provider-health state, before/after cache measurements, warnings or
errors remaining in production logs, and every action that required user approval.
