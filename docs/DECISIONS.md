# ai.ubq.fi Decisions

Read before changing model routing, cache-read telemetry, VPS acceptance, lint configuration, or Deno app inventory.
These are scoped decisions, not general policy; they narrow global defaults only for this repository and never weaken
higher authority.

Provider routing decisions are maintained separately in `docs/provider-decision-journal.md`.

## Terminal truthfulness questions are settled - 2026-09-21

The generalized terminal-truthfulness program asked two specification questions before any stop-reason mapping could be
written. Both are answered from primary OpenAI specification sources, and the merged mapping depends on the answers, so
they are recorded here rather than left open in the handoff.

**Q1 - the incomplete reason vocabulary.** The current Responses schema defines `incomplete_details.reason` as exactly
`max_output_tokens`, `max_messages`, `content_filter`, `steered`, and the response `status` enum as
`completed | failed | in_progress | cancelled | queued | incomplete`. The reasoning guide states that reaching either
the context-window limit or `max_output_tokens` yields `status: "incomplete"` with
`incomplete_details.reason: "max_output_tokens"`. Therefore output-budget exhaustion and context-window exhaustion share
`max_output_tokens`; no separate context reason is invented, and the mis-spelled `max_tokens` found in one
streaming-reference example is never emitted.

**Q2 - reasoning-only output with stop reason `stop`.** This is not a specified incompletion, so no incomplete reason
may be fabricated for it. Under this gateway's own declared route contract, a stream that accumulated no tool call, no
non-empty assistant text and no refusal is an unusable completion, and it is classified with the existing
`empty_upstream_completion` failure kind rather than as `response.incomplete`. An explicit upstream truncation or
filtering signal is an incompletion and wins over that classification.

Reason: without a recorded answer, the next provider adapter would re-derive the rule, and the two plausible readings
differ in exactly the case (reasoning-only `stop`) that the gateway now classifies. The reason spelling is a wire
contract, so `max_output_tokens` is not a stylistic choice.

Reversal risk: emitting `max_tokens`, inventing a context-specific reason, or reporting a reasoning-only `stop` as
either a clean completion or an incomplete response would each restate a fact the specification does not support, and
would silently change what clients and operators read from a terminal event.

## Codex honors response.incomplete, measured end to end - 2026-09-21

The generalized mapping emits `response.incomplete` with `incomplete_details.reason` for a truncated generation. Whether
that is a safe change depends on how the actual Codex client reacts, which no amount of reading the gateway can settle.
It was therefore measured directly with a controlled A/B: two byte-identical Responses SSE streams that differ only in
their terminal event, served to `codex-cli 0.155.1` as a configured Responses provider.

| Terminal served                                   | Codex output                                                                                                             | Exit | `task_complete.error` |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ | ---- | --------------------- |
| `response.incomplete`, reason `max_output_tokens` | the streamed text, then `stream disconnected before completion: Incomplete response returned, reason: max_output_tokens` | 1    | that message          |
| `response.completed`, same text                   | the streamed text, no error                                                                                              | 0    | none                  |

Each run made exactly one HTTP request to the scripted upstream, so an incomplete terminal causes neither a silent
acceptance nor a retry loop.

Two consequences are recorded here so they are not re-derived:

- **The mapping is safe to keep.** Codex parses the incomplete terminal natively, surfaces the reason string verbatim,
  and fails the turn. A truncated generation therefore moves from "accepted as success with exit 0" to "reported with
  the reason and exit 1", which is the intended trade rather than a regression.
- **The mapping is already opt-in per provider, so no filter is needed.** `deepSeekFinishDisposition` is the only
  constructor of `response.incomplete` in the repository, and it is consumed only by the DeepSeek Responses translator.
  Surplus, OpenLux, and the Codex upstream route through their own paths and cannot inherit it. Adding a provider
  inherits nothing; a provider that should use it needs its own deliberate mapping.

Reversal risk: removing the mapping restores the silent truncation, and reading a non-completed terminal as a transport
failure would discard a partial answer the client can still use. Do not add a per-provider allow/deny list for this
mapping on the assumption that it leaks across routes; it does not.

Residual gap: Surplus and OpenLux were not probed, so their truncation-stop behaviour remains unverified. Codex's
handling of the terminal is proven; which upstreams ever emit it is not.

## Narration-without-action is model-specific and context-gated - 2026-09-21

The investigation that produced the terminal-truthfulness work began with a model believing its turn completed mid-task:
it narrates the next action in text and terminates without emitting the tool call it described. Earlier measurement
could not reproduce that shape and reported the gateway as faithful (a tool call in 19 of 20 requests). That measurement
was taken at a context size far below the real sessions, which is why it came back clean.

**Measured with a context-size sweep.** One payload family, identical tool schemas and identical conversation, varying
only the amount of prior tool history; nothing else differs between the two models except the id and the reasoning
effort. Ten runs per cell, work outstanding, classification by whether a `function_call` item was emitted:

| Model                           | Actual input tokens |  n | Emitted tool call | Narrated and stopped |    Rate |
| ------------------------------- | ------------------: | -: | ----------------: | -------------------: | ------: |
| `deepseek-flash` (effort `max`) |                 951 | 10 |                10 |                    0 |      0% |
| `deepseek-flash` (effort `max`) |               4,400 | 10 |                 7 |                    3 |     30% |
| `deepseek-flash` (effort `max`) |               9,003 | 10 |                 6 |                    4 |     40% |
| `deepseek-flash` (effort `max`) |              14,186 | 10 |                 9 |                    1 |     10% |
| `deepseek-flash` (effort `max`) |              18,787 | 10 |                 5 |                    5 | **50%** |
| `deepseek-flash` (effort `max`) |              28,558 | 10 |                 6 |                    4 |     40% |
| `deepseek-flash` (effort `max`) |              66,533 | 10 |                 5 |                    5 | **50%** |
| `gpt-reserve` (effort `medium`) |              61,005 | 10 |                10 |                    0 |      0% |
| `gpt-reserve` (effort `max`)    |              61,005 | 10 |                10 |                    0 |      0% |
| `gpt-reserve` (effort `medium`) |             175,888 | 10 |                10 |                    0 |      0% |

Every run in every cell terminated `response.completed`; the difference is only whether a tool call accompanied it.

Two conclusions follow, and they are the reason this entry exists:

- **It is a model behaviour, not a gateway defect.** At the same ~66k context with the same payload, DeepSeek drops the
  tool call half the time and `gpt-reserve` never does — including at 175k, nearly three times the DeepSeek band. A
  translation or transport defect in this gateway would not spare one provider and hit the other on an identical body.
- **Context size is the trigger, and the onset is between roughly 1k and 4k input tokens.** Fine-grained bands place it
  lower than first recorded: 0% at 951 tokens, then 30% already at 4,400. Above that onset the rate is flat and noisy
  across 4k-67k (30 / 40 / 10 / 50 / 40 / 50%) with no monotone trend, pooling to 38% across all bands above 1k. A
  Fisher exact test of the tiny band against everything above it gives `p = 0.025`. It is a step change, not a gradual
  degradation, and the earlier clean result was a correctly executed experiment at the wrong scale.

The real symptomatic sessions ran at a median of about 485k input tokens, far above the onset, which is consistent with
154 of 292 turns ending in narration there.

Reason: the honest scope of the merged fix depends on this distinction. The terminal work makes the outcome _truthful_
(`response.completed` carrying no tool call is reported accurately instead of being laundered), but no gateway change
can make the model emit the call it decided to describe and skip. Recording the model-versus-gateway separation, with
the control that establishes it, prevents a future reader from either re-deriving it or "fixing" the gateway for a
behaviour it does not cause.

Reversal risk: treating this as a gateway defect and adding gateway-side tool-call requirements or prose heuristics
would fire on legitimate completions that end with forward-looking wording, and would misattribute an upstream model
behaviour to the transport layer.

**Effort is not the variable.** The control was re-run at effort `max`, matching DeepSeek exactly on the same payload:
10 of 10 tool calls, 0% narration at the same 61,005 input tokens, identical to its `medium` result. So the model
difference survives effort being held constant, and reasoning effort is ruled out as the cause.

**Effort does not show a detectable effect on the DeepSeek side either.** The curve was swept at the ~66k band across
`max`, `high`, `low` and `none` (10 runs each): 50%, 20%, 30%, 60% narrated. Those point estimates look like a trend and
are not one — every pairwise Fisher exact comparison among the four levels is non-significant (`p` from 0.17 to 1.00):

| Comparison       | Narrated     |     p |
| ---------------- | ------------ | ----: |
| `max` vs `high`  | 5/10 vs 2/10 | 0.350 |
| `max` vs `low`   | 5/10 vs 3/10 | 0.650 |
| `max` vs `none`  | 5/10 vs 6/10 | 1.000 |
| `high` vs `low`  | 2/10 vs 3/10 | 1.000 |
| `high` vs `none` | 2/10 vs 6/10 | 0.170 |
| `low` vs `none`  | 3/10 vs 6/10 | 0.370 |

The **model** difference, by contrast, is solid on the same data: DeepSeek pooled across effort narrated 16 of 40 (40%)
against `gpt-reserve` 0 of 20 (0%), Fisher exact `p = 0.0005`; restricted to the directly matched `max`-vs-`max` cells,
5/10 against 0/10, `p = 0.033`.

Reason for recording the non-result: the four DeepSeek point estimates could easily be read as "lower effort helps" or
"higher effort hurts", and neither is supported. n = 10 per cell does not resolve differences of this size, so a future
reader should not tune reasoning effort on the strength of those numbers.

Reversal risk: selecting or advertising a reasoning tier as a narration mitigation, or dismissing the model difference
because a single-effort cell happened to look clean, would each act on noise rather than on the measured effect.

Residual limits: rates are point estimates from 10 runs per cell, and the intermediate bands are individually noisy
enough that only the onset (between ~1k and ~4k) is established rather than a precise threshold. The effort sweep is
underpowered to exclude a small effort effect.

## Per-upstream truncation coverage for the terminal mapping - 2026-09-21

The terminal-truthfulness work changes what a truncated generation reports. Whether that is safe per provider was
checked provider by provider rather than assumed from one implementation, because the mapping lives inside a route.

**Only one construction site exists.** `response.incomplete` is built at exactly one place, `src/deepseek_responses.ts`
(the DeepSeek translator), and is reachable only from `handleDeepSeekChatCompletions` and `handleDeepSeekResponses`. No
other provider route can emit it. A per-provider allow/deny filter would therefore be solving a leak that does not
exist; a provider that should use the mapping needs its own deliberate implementation.

| Provider | Reachable | Truncation behaviour                                                                         |
| -------- | --------- | -------------------------------------------------------------------------------------------- |
| DeepSeek | yes       | `length` maps to `response.incomplete` with `incomplete_details.reason: "max_output_tokens"` |
| Cerebras | yes       | Reasoning-only truncation fails closed as `cerebras_upstream_invalid_response` (502)         |
| Surplus  | no        | HTTP 402 `insufficient_credit`: "Insufficient balance to fund this request from prepaid"     |
| OpenLux  | no        | `local:insufficient_quota`: "user quota is not enough"                                       |

**The Cerebras path was reproduced, not inferred.** A direct probe with `max_completion_tokens: 16` returned
`finish_reason: "length"` with the `content` key absent entirely and only `reasoning` populated (53 characters), on two
consecutive runs. That trips `choiceHasNoPayload` (`src/cerebras.ts:376`, applied at `:403`), which rejects a choice
carrying neither content, nor a tool call, nor a refusal. Through the gateway the same request returns HTTP 502
`cerebras_upstream_invalid_response`, recorded in the error ledger as
`chat.completions 502 cerebras_upstream_invalid_response model=gpt-oss-120b`. Note that reasoning alone is deliberately
not sufficient payload: it is preserved for clients as `message.reasoning`, but it is not content.

Reason: this is the contrast the whole program turns on. Cerebras already refused to call a reasoning-only truncation a
success while the DeepSeek route reported the equivalent outcome as a clean completion. Recording the reproduced
mechanism keeps that contrast as evidence instead of as an argument.

Reversal risk: treating reasoning as payload, or reporting a reasoning-only truncation as a completed generation, would
restore the silent truncation on both routes.

**Residual gap, stated rather than closed.** Surplus and OpenLux are blocked on external account state, so their
truncation behaviour is unverified. The gap is narrower than "unknown": neither can currently emit the incompletion at
all, so the untested surface is empty until either is deliberately wired in. Probe both before trusting either, and
recheck the blockers before treating them as permanent.

## DeepSeek adapter deliberately diverges from the vendor client - 2026-09-21

Four DeepSeek interpretations were compared against the provider's own first-party client
(`@deepseek-ai/dsh-llm-deepseek` 0.1.1-rc.2, `lib/index.js` SHA-256
`eed9492246cc6451f060de211768d3128388046478deae7f1959de7cde56ea82`) and are deliberately kept as they are. Do not "fix"
this gateway toward the vendor client on these four points; each difference serves a different contract.

- **Reasoning replay scope (Delta 4).** The vendor replays `reasoning_content` on every reasoned assistant turn. This
  gateway fills an empty string only on the tail after the last `user` message. Probed 2026-09-21 across four history
  shapes: the provider accepts a later `user` turn resetting the boundary, so the narrower rule satisfies the same
  requirement with the smallest mutation. Widening the fill would change token accounting and cache behavior on every
  request. A related research claim that replay is required "from all prior turns" is contradicted by those probes and
  must not be used to widen the fill.
- **Disabling thinking (Delta 5).** The vendor sends `thinking: { type: "disabled" }`; this gateway sends
  `reasoning_effort: "none"`, which the provider documents as the same thing ("`none` disables thinking mode"). Probed
  equivalent on the response path. The vendor's form is not part of the OpenAI Chat Completions schema, and this route
  exists to be OpenAI-compatible, so the documented OpenAI-shaped field stays. Re-probe if the provider changes the
  toggle.
- **Usage accounting (Delta 6).** The vendor subtracts cache reads because its internal convention is disjoint counts.
  This gateway relays `prompt_cache_hit_tokens` as the official `prompt_tokens_details.cached_tokens`, which the OpenAI
  contract defines as a subset detail of `prompt_tokens`. Subtracting here would corrupt the field's meaning for every
  OpenAI-compatible reader. This is the same rule as "Cache-read telemetry is reported, never defaulted" (2026-09-19).
- **Translation seam exists by design (Delta 9).** DeepSeek now serves a native Responses endpoint (`POST /responses`
  and `POST /v1/responses`, both HTTP 200 with a native envelope, probed 2026-09-21), so the translation layer is no
  longer required by a provider gap. It stays because the native endpoint is documented as stateless with several
  control parameters ignored and because this adapter's measured `reasoning_content` fill for tool-bearing tails is a
  provider requirement a native response would have to reproduce. A migration is a separate evidence-driven evaluation,
  not an assumed cure.

Reversal risk: reverting any of these four toward the vendor client reintroduces a contract mismatch (wider cache
accounting, a non-OpenAI parameter on an OpenAI-compatible route, changed token/cache behavior on every request), or
re-justifies the translator with a premise that is no longer true.

## DeepSeek thinking mode rejects two tool_choice values - 2026-09-21

DeepSeek answers HTTP 400 `Thinking mode does not support this tool_choice` for `tool_choice: "required"` and for the
named-function form whenever thinking mode is active. Probed 2026-09-21 on `POST /chat/completions` at `low`, `high`,
and the omitted default, on the provider's native Responses endpoint, and in the accepting direction with
`reasoning_effort: "none"` and `thinking: { type: "disabled" }`; `none` and `auto` are accepted in every mode. The
official Chat Completions reference documents the restriction ("`required` and named tool choices are not supported in
thinking mode; the API returns a 400 error").

The gateway therefore rejects the incompatible combination at its own boundary on both DeepSeek request seams, with one
shared predicate and an error naming both conflicting fields, instead of forwarding the request and relaying the
provider's message about a parameter the gateway's own contract advertises.

Reason: a client cannot know from this gateway's advertised capabilities that a supported `tool_choice` combined with a
supported reasoning tier is invalid. The boundary is the only place that sees both.

Reversal risk: removing the guard restores a live client-visible 400 whose message blames the caller for a combination
the gateway accepted, and re-opens the gap on both seams.

## Reserve Codex model id with its own quota class - 2026-09-20

`gpt-reserve` is luna served under a second Codex model id the owner authorized on 2026-09-20 as a distinct model with
its own quota limit, so it is not a gateway-only alias: the requested id is passed upstream verbatim and is never
renamed to `gpt-5.6-luna`. It owns the `reserve` quota class in `src/codex_account_routing.ts`, so exhausting the
reserve class must not block the standard class on the same account, and standard-class exhaustion must not block
reserve. The gateway accepts the id as a known Codex model while the upstream discovery catalog still omits it, without
inventing a catalog entry.

Reason: the upstream serves the id today but its discovery catalog may lag, and whether the upstream meters reserve
separately is unproven. Keeping the id verbatim and giving it its own durable bucket leaves the distinction observable
instead of folding a possibly separate limit into the standard class.

Reversal risk: renaming the id upstream, folding `reserve` back into `standard`, or widening the trusted accepted-id
list beyond the owner-authorized id each removes the distinction this authorization depends on.

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

Summary only: `docs/provider-decision-journal.md` owns this provider-routing decision (entry dated 2026-09-14, "Serial
subscription routing"). Exhaust one Codex subscription before advancing the provider chain; never spread ordinary
requests across subscriptions while the active one still has capacity.

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
