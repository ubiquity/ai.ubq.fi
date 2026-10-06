# The `ubiquity/deepseek-v4.1-flash` waterfall model — implementation plan

Decision: one synthetic model id that the gateway serves by walking the measured provider order on the client's behalf.
Agents keep addressing one id; the gateway owns provider choice, failover, and evidence.

## Identity and wire

- Model id: `ubiquity/deepseek-v4.1-flash`; display name: `Ubiquity DeepSeek V4.1 Flash`; description states the
  automatic provider fallback.
- Wire: `/v1/responses` only (the subagent wire). Chat Completions is out of scope for phase 1.
- Provider-specific ids stay unchanged: `deepseek/deepseek-v4.1-flash` (OpenRouter), `deepseek-ai/DeepSeek-V4.1-Flash`
  (LithosAI), `deepseek-flash` (official), `deepseek-v4.1-flash` (paid catalogue for Surplus). Operators keep the
  existing provider-selection pinning, so benchmark batches can still force a single provider.

## Order and semantics

Order: **OpenRouter → LithosAI → DeepSeek direct** in phase 1; **Surplus** joins as the final hop in phase 2 through the
paid pipeline (surplus-only, because the metered/OpenLux model record cannot serve the Responses wire).

Per request the gateway attempts each enabled provider in order and advances only on infrastructure/serving failures:

- transport/connect/DNS failure, request timeout, HTTP 429, HTTP 5xx, HTTP 402/403 quota or capacity. Phase 2 adds
  pre-semantic stream failures (malformed SSE, interrupted stream, missing completion before any output).
- Never advance on a client-caused 4xx validation response or on a valid-but-wrong answer.
- Once any semantic output has reached the client, failover stops: the gateway reports the failure and the client's
  retry restarts the chain at the top. The attempt-level integration that would cover mid-stream failover is phase 2.
- Each hop uses the existing bounded first-event deadline; the chain is bounded by the request deadline, and a hop is
  skipped when the request signal is already aborted.

## Evidence and observability

- Every attempted provider is recorded in the terminal `attempted_providers` list; the failing status or error class is
  retained for the chain, so production traffic keeps measuring the waterfall (objective section 12).
- The delivered response keeps `x-uos-upstream` = the provider that actually served, and the terminal log keeps the
  request identity.

## Capabilities and metadata

- Context window 1,048,576 tokens on every candidate; tools supported on every candidate.
- Advertised reasoning tiers are the intersection the candidates all accept verbatim: `low`, `high`, `max`. The
  waterfall boundary rejects any other value for this id with a 400 instead of letting a provider refuse it later.
  (DeepSeek official also accepts `none`; Surplus/OpenRouter advertise only low/high/max, so `none` is not advertised.)
- The Codex-facing catalogue record mirrors the LithosAI record shape; the plain `/v1/models` list also carries the id.

## Phases

- **Phase 1 (this change):** resolver + wrapper over the three direct handlers, catalogue record, reasoning validation,
  fallback telemetry, hermetic unit tests, and a live smoke of every hop through provider-selection pinning.
- **Phase 2:** Surplus as the final hop through the paid pipeline (admission, reservation, ledger settlement must not be
  bypassed), and pre-semantic mid-stream failover by integrating with the attempt primitives (`prepareResponsesAttempt`
  and the precommit buffering the paid route already uses) instead of handler-level wrapping.
- **Phase 3 (optional):** explicit per-provider pin ids for experiments; not needed for production.

## Rollback

Remove the catalogue record, the dispatch branch, and the waterfall module. Provider-specific ids, paid-tier routing,
and the Codex waterfall are untouched by construction.

## Acceptance

- Hermetic tests: order resolution with disabled providers, failover on 5xx/429/402/403, no failover on 400, rejection
  of unadvertised reasoning tiers, catalog record shape.
- Live smoke (local benchmark instance, provider selection pinned per hop): default selection serves via OpenRouter;
  pinning to `["lithos"]` / `["deepseek"]` serves via those providers through the synthetic id; the terminal log records
  the served provider.
- Repository gate: `sh scripts/verify.sh` plus the CI checks on the pull request.
