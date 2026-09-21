# ai.ubq.fi Decisions

Read before changing model routing, cache-read telemetry, VPS acceptance, lint configuration, or Deno app inventory.
These are scoped decisions, not general policy; they narrow global defaults only for this repository and never weaken
higher authority.

Provider routing decisions are maintained separately in `docs/provider-decision-journal.md`.

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
