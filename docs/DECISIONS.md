# ai.ubq.fi Decisions

Read before changing model routing, cache-read telemetry, VPS acceptance, lint configuration, or Deno app inventory.
These are scoped decisions, not general policy; they narrow global defaults only for this repository and never weaken
higher authority.

Provider routing decisions are maintained separately in `docs/provider-decision-journal.md`.

## Codex premature turn endings: real reproduction, and the continuation-guidance contract - 2026-09-22

The case that Codex can end a turn with an outstanding requested action is recorded here as reproduced, not inferred
from the invalid frequencies corrected below. The contract this repository now carries is: on the DeepSeek Responses
translation seam, a tool-bearing request whose mapped executable tools exist and whose `tool_choice` is not `none` gets
one short neutral continuation instruction appended to the caller's own instructions, or carried as the system message
when the caller sent none. It never forces a tool call, forbids a legitimate final answer, or claims a tool action
happened.

The reproduced pair, one real `codex-cli 0.155.1` run per variant against a task-owned loopback bridge serving the
Responses wire API, same fixed cwd, same provider id, same captured native catalog, same model `deepseek-flash` at
effort `max`, and the same 16-step read-only chain prompt (receipt
`591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/509ce225-f066-4bf6-af9c-fcd85739173f`):

| Variant                           | Reads                    | Terminal state                                                                                                                                                                                                                                     | Exit | Checksum |
| --------------------------------- | ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- | -------- |
| baseline (`a4d0b470`)             | 11 of 16 files, in order | one `turn.completed`, final text `Step 12 of 16, reading nodes/2c/rotate-4571.snippet`; last upstream call HTTP 200, `finish_reason` `stop`, zero tool calls, 20 completion tokens of an 8192 allowance, `[DONE]` present, no error, no truncation | 0    | absent   |
| candidate (continuation guidance) | 16 of 16 files, in order | one `turn.completed`, one marker per successful tool result, no human confirmation and no goal auto-continuation                                                                                                                                   | 0    | 3961     |

The probe's own `incomplete-chain` classification wins its check order, and its `earlyTextOnlyStopObserved: false` is a
consequence of that ordering; neither refutes the captured upstream facts above.

Scope and limits, so the claim is not overstated: this is one paired long run, so it establishes that the behaviour
occurs and that in this pair the baseline stopped while the candidate completed; that supports the reminder as a
mitigation, not a rate, a probability, a universal cure, or a causal claim from n = 1. The mechanism that makes the
model stop mid-chain is still not identified. A frozen historical replay did not reproduce the stop, and a real-Codex
phase-only A/B ended after one request for both `commentary` and `final_answer`, so changing output phase alone did not
make Codex continue. This change does not modify phase, and that phase-only test says nothing about upstream
history-phase effects. Related but non-causal measurements: the served native catalog's `deepseek-flash` entry has empty
`base_instructions` and the native client sends `instructions: ""` with the developer/system input text still present;
that catalog fact alone is not claimed as the cause. The vendor `thinking` field is now accepted on DeepSeek and
projected to `reasoning_effort` (PR #391, the base revision for this measurement), and that compatibility change did not
fix these premature stops.

Reversal risk: the guidance is appended to every DeepSeek tool-bearing request that permits tool use, so removing it
restores the measured baseline, while widening or rewording it can make the model prefer tool calls over a legitimate
final answer or pad otherwise ordinary agent traffic. The truthfulness, `tool_choice: none`, non-agent and
legitimate-final cases in `tests/deepseek-responses.test.ts` preserve the request and terminal contracts; they do not
detect a model's semantic preference, and the real-client controls are what check that a legitimate final answer is not
displaced.

Residual obligation: the visible behaviour above was measured against a task-owned loopback. Loopback evidence and
served-release acceptance are distinct: the exact served identities, and the actual client outcomes against them, must
be recorded in the release handoff, using the served-client probe beside this entry's evidence directory.

### Follow-up: the reminder alone is insufficient, and the semantic recheck - 2026-09-22

The PR #395 reminder was merged and deployed as `e04f67ff`. The VPS real 16-step run passed, but on the Mac the real
client stopped at 10 of 16 with final text `step 11 of 16, reading nodes/7b/tally.sql`, exit 0, one completed turn, and
receipt `591bceabb6cc0ae63ee09ee9914b02c17ad0b9b53f9be3f4389670cde15755a5/cb7c10e7-08d5-41b3-8e0b-e421cdaa969a`. That
outcome establishes that the reminder alone is insufficient. The original paired reproduction stays recorded as
historical evidence of the baseline stop, and it must not be recast as proof that the reminder never works.

The replacement contract is one semantic recheck on a successful, text-only stop where mapped executable tools exist,
`tool_choice` is automatic, the stop carries no refusal, usage is known, and positive output remains. It runs the same
model with the same tools and effort, adds no wire fields or settings, buffers the recheck, keeps the first stream
progressive and keeps the original response identity; accepted returned tools are delivered before the terminal event
and no duplicate recheck text is emitted. The original answer is preserved on a legitimate final answer, on no tools,
and on advisory failure. The guard skips the recheck for `none`, `required`, or named tool choice, truncation, empty
output, a refusal, no tools, and unknown or exhausted budget. Cancellation aborts the extra call, and there is no second
admission or reservation.

One extra provider request is the cost for an eligible text-only final: input cost and latency rise, and the output cap
is `min(remaining original allowance, 8192)`. Actual usage from both requests is summed; missing fields stay partial and
no cache zeros are invented. Refusal metadata is preserved through provider normalization solely so the guard can
observe it, with no new refusal rendering behaviour.

This is a bounded mitigation, not a guarantee against the model's choice after both passes, and runtime acceptance of
the new recheck is still pending, so it must not be described as deployed or passed. The regression tests cover
protocol, usage, budget, and cancellation behaviour; the real 16-step checks are the model-behaviour evidence, not the
reverse.

## App-wide visual language follows the deno-universal-auth reference - 2026-09-22

The app-wide design is the shared token system in `static/style.css`, ported from the `deno-universal-auth` reference
contract: OS-driven light and dark, system-UI type at 14px body and 20px headings, 10px control and 14px panel radii,
44px controls, quiet shadows, and blue reserved for actions and selection.

- Palette, light: `--bg` `#f7f9fc`, `--surface` `#ffffff`, `--surface-2` `#f1f5f9`, `--surface-3` `#e2e8f0`, `--text`
  `#111827`, `--muted` `#5f6b7a`, `--muted-2` `#55606e`.
- Palette, dark: `--bg` `#09090b`, `--surface` `#141418`, `--surface-2` `#1d1d22`, `--surface-3` `#29292f`, `--text`
  `#f5f5f7`, `--muted` `#a1a1aa`, `--muted-2` `#8e8e99`.
- Light is the base block and `@media (prefers-color-scheme: dark)` overrides only the palette; `color-scheme` stays
  native (`light dark`) and no page declares a theme class, toggle, or stored preference.
- Actions use `--accent` `#0063d1`, `--accent-hover` `#006fe6`, and white `--accent-ink`; dark uses `#0a6ae0` and
  `#0f70e0` with the same white label, while the brighter `--link` `#4da3ff` and translucent `--selection` carry dark
  link, focus, and selected states.
- The contrast repairs are deliberate: the reference's `#007aff`-on-white action, `#0a84ff`-with-white dark label, and
  1.3:1 `#d5dee9` input border are replaced, so quiet text clears 4.5:1 and `--input-border`, `--border-strong`, and
  `--focus-ring` clear 3:1.
- Page styles consume `--radius-sm` for controls, `--radius-lg` for panels, and `--control-height` instead of
  redeclaring a palette; no page adds a second alias token system.
- Motion uses `--duration-fast` at 140ms for press and hover feedback and `--duration-med` at 220ms for occasional
  surface changes; hover motion is gated by `(hover: hover) and (pointer: fine)`, keyboard actions stay immediate, and
  reduced motion keeps short fades while dropping transforms.

Reason: the app was dark-only, with hardcoded `rgba(255,255,255,...)` surfaces through the shared sheet, a pinned
`color-scheme: dark`, and a visual contract that lived only in `tests/static-assets.test.ts`. Recording the tokens, the
light and dark ownership, and the contrast repairs here keeps the next page-local restyle from re-deriving a divergent
palette.

Reversal risk: a page-local `:root` palette, a pinned `color-scheme: dark`, or a light-only literal re-splits the app
and restores the measured contrast failures, and deleting the token assertions in `tests/static-assets.test.ts` removes
the only automated guard because no browser or screenshot check exists in the repository.

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

## Deployment status of the terminal-truthfulness work - 2026-09-21

The measurement-artifact correction above retracts the premise that motivated this work. It therefore matters which
parts are actually running, and whether any deployed behaviour change was justified by the retracted claim.

**Status update, same day: both deployments now run the program.** At the time this entry was first written none of it
was deployed, and the two running releases predated every commit in it. The program was then deployed to both surfaces
at `2207a757fb6f8c362e18cbf890d59ee191d53e26`, after CI passed on that exact SHA and `verify: OK` on the same revision:

| Deployment                                                 | Release      | Identity                                       |
| ---------------------------------------------------------- | ------------ | ---------------------------------------------- |
| VPS (production, and the public `https://ai.ubq.fi` route) | `2207a757fb` | `vps-2207a757fb6f8c362e18cbf890d59ee191d53e26` |
| Mac local (`localhost:7999`)                               | `2207a757fb` | `mac-2207a757fb6f8c362e18cbf890d59ee191d53e26` |

Acceptance ran against the deployed releases, not the branch:

- the truncation reproduction returns `response.incomplete` with `incomplete_details.reason: "max_output_tokens"` and
  `output_tokens: 8192` on both hosts, where it previously returned `response.completed` with
  `incomplete_details: null`;
- three normal completions still return `status: completed` with `incomplete_details: null`, so it does not over-fire;
- authenticated inference through the public route succeeds, and `gpt-reserve` serves on both hosts.

The retracted premise is now moot in one direction: the deployed G3 is narration-independent by construction - it fires
only when a would-be completion carries no assistant text, no refusal and no tool call, so it cannot act on the
retracted symptom. Worth revisiting before any future change: the changelog wording that described it as a "no tool
call" guard, which reads as narration-targeted although the code does not test for that alone.

Prior state, retained: the two then-running releases (`922c33392d` on the VPS, `4176e992f5` on the Mac) preceded every
commit in the program, so at that time no production inference behaviour had changed.

**Only one of the three changes depends on the retracted premise, and partially.**

- **G5 (report the effective output allowance)** - independent. It is telemetry only, changes no generation behaviour,
  and was justified by the provider's tier-dependent default (8,192 at `none`), measured directly.
- **G1 (`length` to `response.incomplete`)** - independent. It was reproduced end to end: an upstream
  `finish_reason: "length"` reached the client as a clean `response.completed` with `incomplete_details: null` while the
  gateway's own telemetry recorded `output_tokens: 8192`. That demonstration does not involve narration at all.
- **G3 (fail closed on a degenerate completion)** - **partially dependent.** Its stated trigger is a stream about to
  complete "with no tool call, no non-empty assistant text and no refusal". The no-tool-call clause was written for the
  narration symptom that has now been retracted. The reasoning-only clause stands on its own: a completion whose only
  output is reasoning hands the client nothing, which the Cerebras route already failed closed for independently of any
  narration claim.

Reason for recording this: the correction above removes the motivation for one clause of one change, and a future reader
deciding whether to deploy needs to know that the other two changes and most of the third rest on reproductions that
survived the retraction.

Reversal risk: deploying on the belief that the narration symptom is real, or reverting the whole program because its
original motivation was retracted. The first is unfounded; the second discards two independently reproduced fixes.

Next action if the program is to be deployed: re-derive G3's trigger from the surviving evidence alone - keep the
reasoning-only and empty-output clauses, and justify or drop the no-tool-call clause on its own merits rather than on
the invalidated frequency claim.

## CORRECTION: the narration symptom was itself a measurement artifact - 2026-09-21

> **Superseded in part on 2026-09-22.** The reported counts remain unreproducible and cannot be quoted as a rate. A
> paired real-client reproduction did observe one premature stop with outstanding work, so these invalid counts are not
> evidence that the behaviour is absent either. See the entry at the top of this file.

The entry below and the terminal-truthfulness handoff both rest on an observed condition: a long-running agent
"frequently believes its turn completed mid-task", quantified as 154 of 292 turns in one session and 24 of 67 in
another. **Those reported counts are not reproducible from the recorded sessions.** It appears to be an artifact of how
the original count was taken, and the entries that depend on it should not be cited as evidence that the behaviour is
common.

**The numbers do not reconcile.** For the two sessions the handoff names, counting every plausible unit:

| Session                | `task_started` | `task_complete` | assistant text messages | matching a forward-looking phrase |
| ---------------------- | -------------: | --------------: | ----------------------: | --------------------------------: |
| `sentinel`             |            423 |             414 |                   1,053 |                               805 |
| `oracle-free-arch-vps` |            145 |             143 |                     497 |                               269 |

The handoff reports 292 turns / 154 narrated for `sentinel` and 67 / 24 for `oracle`. **No column matches either
figure**, so the original measurement cannot be reconstructed from the sessions it cites.

**The forward-looking-phrase test does not identify premature endings.** Inspecting the messages it matches shows they
are ordinary mid-task narration that is _followed by a tool call_ — "Let me load the required harness policy and verify
key facts in parallel", "Let me answer the ai.ubq.fi question definitively". The phrase appears in 76% of all assistant
messages (805 of 1,053), which is why the test is unusable as a discriminator: it fires on normal working text, not on a
malfunction.

**What the turns actually look like.** Reading turns to their end shows the model doing hundreds of tool calls and then
closing with a substantive summary. The 4211- and 6380-character closers on `task_complete` are honest completions of
long investigation work, not a model believing it finished early. Turns cluster at a median of 7 response items, with
only 3 of 13 in one sampled window exceeding 10 items.

Reason for recording this: two merged documents and six merged corrections treat this symptom as established and build
on it. A future reader must know that the premise is unsupported, or the chain of reasoning above this entry inherits an
artifact. The gateway trustworthiness findings in the handoff stand on their own evidence and are unaffected; only the
claim that the symptom is frequent, and the model-versus-gateway contrast drawn from it, depend on this.

Reversal risk: quoting 154-of-292, treating forward-looking phrasing as a malfunction signal, or building any detector
on that regex. Each propagates the artifact.

Method note: the original count was never reproduced, so the defect is most likely in the counting procedure rather than
in the sessions. Any replacement measurement must state its unit (turn, message, or item), its window, and how it
decides a turn was premature, and must show that the classifier does not fire on ordinary mid-task narration.

## CORRECTION: the narration trigger is not context size - 2026-09-21

The entry below reports that context size gates narration-without-action. **That is falsified.** It is retained for
history, but the trigger section and the onset threshold must not be relied on. The reason is an external-validity
failure in the experiment: every payload used to derive the curve repeated one identical filler block in every tool
output, so "long context" and "degenerate repetitive context" were confounded and could not be separated.

**The disconfirming measurement.** A second payload family was built with genuinely distinct tool outputs (20 rotating
result shapes plus per-index text, every output unique) and matched to the original on byte size and item count. Both
were run against `deepseek-flash` at matched effort:

| Payload               | Actual input tokens |      n | Narrated |   Rate |
| --------------------- | ------------------: | -----: | -------: | -----: |
| repetitive (original) |              66,533 |     28 |        8 |    29% |
| **varied (matched)**  |          **71,295** | **18** |    **0** | **0%** |

The varied payload is _larger_ than the repetitive one and narrates _never_. Length alone therefore cannot be the
trigger, which falsifies both "context size is the trigger" and the recorded onset of "between roughly 1k and 4k".

**A controlled interleaved run, and its non-result.** To separate condition from time drift, the three conditions
(repetitive ~66k, varied ~71k, small ~1k) were cycled round-robin inside a single time window, eight rounds:

| Condition       | Narrated in the interleaved window |
| --------------- | ---------------------------------: |
| repetitive ~66k |                          2/8 (25%) |
| varied ~71k     |                           0/8 (0%) |
| small ~1k       |                           0/8 (0%) |

Neither contrast is significant in this design (`p = 0.47` each). Pooling all batches raises the repetitive-vs-varied
contrast to `p = 0.016`, but that pool mixes runs from different time windows and the original effect did not replicate
across batches on its own payload (5/10, then 1/10, `p = 0.14`).

**What survives, and what does not.**

- **Does not survive:** context size as the trigger; the onset threshold; the implication that long real sessions
  narrate _because_ they are long. Real sessions are also full of varied content, so the synthetic confound may explain
  the original 154-of-292 observation as easily as the model does.
- **Weakly survives:** that _some_ conditions produce narration at a low rate. The pooled repetitive cells total 8/28,
  which is not zero. Its true trigger is unidentified; repetitive content is a candidate, not an established cause.
- **Survives:** that `gpt-reserve` never narrated on any payload at any size tested - 0 of 30 pooled across every cell
  run in this investigation, spanning 61k to 176k input tokens and both payload families. The model contrast is weaker
  than first reported, because the DeepSeek rate it is measured against fell, but it has not been contradicted.

Reason for recording at this length: the previous entry states a specific causal trigger with statistics behind it, and
this repository treats that as load-bearing. Leaving a falsified trigger in place would be worse than the correction
itself — a future reader would tune context budgets or payload shapes against a confound.

Reversal risk: acting on the size trigger, quoting the onset threshold, or treating repetitive context as a confirmed
cause. Any of those propagates an experiment artifact.

Method note for the next attempt: vary payload content independently of length, and interleave conditions inside one
time window. Both were missing here, and both are what caught it.

## Narration-without-action is model-specific and context-gated - 2026-09-21

> **Superseded.** The condition this entry measures - a model frequently believing its turn completed mid-task - is not
> reproducible from the sessions it cites; see the measurement-artifact correction above. The context-size trigger and
> onset threshold are separately falsified. Treat every rate here as an artifact. The gateway trustworthiness findings
> in the handoff are unaffected, since they rest on their own evidence.

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

**Recheck 2026-09-22, both blockers still hold.** Surplus answers a probe on a Surplus-routed id with HTTP 402
`Insufficient USDC balance: need ~$1.0000, have $0.9895` (request id `01M33CFBC7AHTBXM365KNCM28D`), and the terminal
ledger records `provider: "surplus"`, `status: 402`, `failure_kind: "read_error"`. OpenLux is still not reached at all:
`gpt-5.6-luna` and `gpt-6-astra` both answer `provider: "chatgpt_codex"` with `fallback_reason: null`, because the Codex
subscription tier serves them first, so the paid tier is never exercised through those ids. The gap therefore remains
open in the same shape, and no truncation evidence was obtained for either provider. Note also that `deepseek-v4-pro` is
served by `provider: "deepseek"`, not Surplus, despite the id appearing in the paid catalogue - so probing that id does
not test Surplus truncation.

## Buffered-terminal fix deployed to both surfaces - 2026-09-22

The buffered DeepSeek Responses fix from PR #387 is live on both surfaces at `3661bcfd13a9c1b9056fe5dc1786bb49b18ad594`,
deployed through `deno task deploy:vps` and `deno task deploy:mac` off CI-green `development`.

| Deployment                                                 | Release      | Identity                                       |
| ---------------------------------------------------------- | ------------ | ---------------------------------------------- |
| VPS (production, and the public `https://ai.ubq.fi` route) | `3661bcfd13` | `vps-3661bcfd13a9c1b9056fe5dc1786bb49b18ad594` |
| Mac local (`localhost:7999`)                               | `3661bcfd13` | `mac-3661bcfd13a9c1b9056fe5dc1786bb49b18ad594` |

Acceptance ran against the deployed releases, not the branch, and covered every DeepSeek wire shape plus the
subscription path:

- buffered `/v1/responses` truncation still reports `response.incomplete` with
  `incomplete_details.reason: "max_output_tokens"` and `output_tokens: 8192` on both hosts;
- three normal buffered completions per host still report `status: "completed"` with `incomplete_details: null`, so the
  new guard does not over-fire;
- the streamed `/v1/responses` path still reaches `response.completed` on both hosts;
- `/v1/chat/completions` (the DeepSeek Harness wire) still answers `finish_reason: "stop"` with content on both hosts;
- `gpt-reserve` still serves on both hosts through the Codex subscription capacity path.

The error ledgers after deployment show no `empty_upstream_completion` fires on either host - the guard has not fired in
production in either direction. The only rows on the new revision are this acceptance run's own truncation
(`max_output_tokens`) and two deliberate 400s from a malformed probe.

`sh scripts/verify.sh` reported `verify: OK` on the merged revision before deployment. Root checkout is clean on
`development`, matching `origin/development`, with the task branches removed.

## Client behaviour on a degenerate completion, and the terminal-truthfulness 502 surface - 2026-09-22

Two questions were left open when the terminal-truthfulness program was recorded: which clients actually fail closed on
the terminals the gateway now emits, and exactly where the program added 5xx responses. Both are answered here from
direct measurement on the merged revision, not from reading the code alone.

**Codex was already measured** (the 2026-09-21 A/B above): `response.incomplete` fails the turn with exit 1 and the
reason string verbatim. **DeepSeek Harness is now measured too**, because the operator uses both and the harness reaches
the gateway over a different wire.

The harness path is `@deepseek-ai/dsh-llm-pi-ai` -> `@earendil-works/pi-ai` `openai-completions`, and the operator's
`ubiquity` provider in `~/.dsh/settings.yaml` is configured with `api: openai-completions` against
`https://ai.ubq.fi/v1/`. Driving that real adapter stack against a scripted upstream (not a reimplementation of it)
gives:

| Wire result the gateway emits                        | Harness outcome                                                                   |
| ---------------------------------------------------- | --------------------------------------------------------------------------------- |
| `finish_reason: "stop"` with reasoning and no answer | `stop` with a `thinking`-only message; **no error, no empty-content guard fires** |
| `finish_reason: "stop"` with `content: ""`           | `stop` with an empty message; **no error**                                        |
| `finish_reason: "length"`                            | `{ kind: "max-tokens" }`, a first-class turn-end reason with its own UI notice    |

So the harness is **not** a backstop for the gateway's completion-validity rule: it maps the wire faithfully and accepts
a degenerate `stop` as a success. Codex is the stricter client of the two. That is the reason the gateway owns G3 rather
than delegating it to the client, and it is why the Chat Completions route keeps its current shape: a client-side
backstop does not exist to lean on.

**The truncated-generation path itself is unchanged for the harness.** `length` maps to `max-tokens` on both the pi-ai
adapter and the vendored `dsh-llm-deepseek` adapter, so a truncation the gateway reports stays reported. The gap is
narrow and one-directional: _degenerate completions_ (`stop` with nothing usable) are invisible to the harness.

**A wire boundary worth knowing before repointing any harness route at this gateway.** Capture of the pi-ai
`openai-completions` request shows the operator's `ubiquity` route sends `model`, `messages`, `stream`, `stream_options`
and `store` - and no `thinking` and no `reasoning_effort`, because the configured model entries declare no reasoning
capability. That traffic is accepted. The vendored `dsh-llm-deepseek` adapter sends `thinking: { "type": "enabled" }`
for any non-`off` effort. **Correction 2026-09-22: that field is no longer rejected.** PR #391 (base revision
`a4d0b470`) accepts the vendor's own `thinking` field on the DeepSeek route and projects it to `reasoning_effort`; it
remains outside the official OpenAI schema everywhere else, so the earlier HTTP 400
`Unrecognized request argument supplied: thinking` no longer describes this route's behaviour. That compatibility change
did not fix the Codex premature turn endings recorded at the top of this file, which remain a separate mechanism.

**The program added no new 5xx responses.** Counting every `openaiError(<n>, ...)` and `streamErrorResponse(<n>, ...)`
call in `src/openai.ts` between the pre-program revision `922c33392d` and the merged terminal-truthfulness revision: 31
five-hundred-and-two calls before, 32 after. The one addition was made on 2026-09-22 by the follow-up below, not by the
program. The only status code the original program itself added anywhere in `src/` was a single `openaiError(400, ...)`
for the DeepSeek thinking-mode `tool_choice` conflict.

Before that follow-up, the three `empty_upstream_completion` 502s were unchanged from their pre-program locations, and
all three were on routes that already had them:

| Site                  | Function                    | Reachable from                                     |
| --------------------- | --------------------------- | -------------------------------------------------- |
| `src/openai.ts:1132`  | `safeFailedAttemptResponse` | Codex Responses attempts (pre-commit)              |
| `src/openai.ts:7901`  | `completeChatCompletions`   | the ordinary Chat Completions path                 |
| `src/openai.ts:10683` | `rejectEmptyChatCompletion` | the shared Chat preflight, not the DeepSeek branch |

The DeepSeek routes bypass `rejectEmptyChatCompletion` by dispatch order: both DeepSeek handlers return before the
shared preflight runs. That is why the original G3 landed as a route-local guard rather than as a reuse of that
function, and it is why the buffered branch below needed its own guard rather than inheriting one.

### The buffered DeepSeek Responses branch was missing G3

Found while answering the two questions above, fixed by PR #387 (`d7472165`), and worth recording because it is the one
place the program was not applied consistently.

The streamed DeepSeek Responses path fails a degenerate completion closed. The **buffered branch of the same route did
not**: a `stop` carrying only reasoning, or only empty content, returned HTTP 200 `status: "completed"` with
`error: null`. The same logical outcome was a failure on one transport and a success on the other.

Reproduced against the real handler with a mocked upstream, before the fix:

    RESPONSES streamed  -> HTTP 200 | terminal: event: response.failed
    RESPONSES buffered  -> HTTP 200 | status: completed | error: null

After the fix the buffered branch returns the ordinary gateway 502 with the existing `empty_upstream_completion` code
and message. Order is preserved: the provider's own reason is read first, so a `length` truncation still returns
`response.incomplete` with `incomplete_details.reason: "max_output_tokens"` and is never converted into the
empty-completion failure. Only a would-be completion is measured for answer-bearing output, through the same one shared
predicate the streamed path and the Chat route consume.

Two facts about why it survived: no test exercised the non-streaming DeepSeek `/v1/responses` path, and the branch is
reachable in production - 10 non-streaming `/v1/responses` requests were served on the deployed revision `2207a757fb`.
The lesson generalises: "the route is covered" is not the same claim as "every branch of the route is covered", and a
single-transport test does not establish a single-route behaviour.

Reversal risk: removing the buffered guard restores the transport-dependent success/failure split; converting an
explicit truncation into the empty-completion failure would break the precedence G1 requires. Both are covered by the
five-step regression test beside the existing streamed G3 test.

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
