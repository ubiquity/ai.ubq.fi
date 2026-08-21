# Codex upstream degradation incident — 2026-08-21

## Summary

From 07:10 through 07:15 EDT on 2026-08-21, the production `ai.ubq.fi` gateway returned a burst of failures for
`/v1/responses` traffic routed through `chatgpt_codex`.

The evidence does not show that OpenAI was globally unavailable. It shows that the gateway's configured `chatgpt_codex`
upstream path stopped returning response headers for several requests. The gateway then opened its upstream-timeout
circuit. A gateway routing defect amplified that upstream degradation: the circuit's `codex_upstream_degraded` 503
response was not eligible for the already configured paid-fallback path, so later requests failed immediately instead of
continuing through Metered or Surplus.

## Impact

The exact 07:10:00–07:15:00 EDT window contained 113 terminal requests:

| Result     | Count | Details                                                                                        |
| ---------- | ----: | ---------------------------------------------------------------------------------------------- |
| Successful |     5 | Completed between 07:10:01 and 07:10:06 EDT                                                    |
| HTTP 504   |     5 | No upstream headers; each reached the 120-second inactivity deadline                           |
| HTTP 503   |   103 | Failed before upstream dispatch after the timeout circuit blocked the configured Codex account |

The fast 503 period began at 07:12:15 EDT and continued through 07:14:53 EDT. Successful upstream attempts resumed
immediately after 07:15 EDT.

Affected requests included `gpt-5.6-luna` and `gpt-5.6-terra` at multiple reasoning levels. Every failed terminal record
listed only `chatgpt_codex` in `attempted_providers`; `fallback_reason` was `null`, and no paid provider was attempted.

## Timeline

- 07:10:01–07:10:06 EDT: Five requests completed successfully.
- 07:12:15 EDT: The first fast, pre-dispatch 503 was recorded.
- 07:12:30–07:14:07 EDT: Five earlier requests reached the 120-second header/inactivity deadline and returned 504.
- 07:12:15–07:14:53 EDT: The gateway returned 103 immediate 503 responses while the Codex upstream circuit was open.
- Shortly after 07:15 EDT: Codex attempts returned 200 again and completed normally.

## Confirmed cause

The incident had two parts:

1. The configured `chatgpt_codex` upstream path did not return response headers for several requests. The available logs
   cannot distinguish a provider-wide incident from an account-specific, network-path, regional, or upstream-service
   problem.
2. The gateway recognized the condition and produced a `codex_upstream_degraded` 503, but
   `fetchResponsesWithPaidFallback` only admitted 401, 403, and 429 outcomes to paid fallback. The degraded-circuit 503
   therefore had no fallback reason and bypassed Metered and Surplus.

The second part caused the large blast radius. It was a gateway defect under our control.

## Repair

The gateway now classifies an HTTP 503 carrying the internal `codex_upstream_degraded` routing error as
`primary_upstream_degraded`. That reason enters the existing paid-fallback admission path.

The repair preserves the existing safeguards:

- paid fallback must be enabled for the calling API key;
- the requested model must be eligible;
- per-key spending and maximum-exposure limits still apply;
- fallback bookkeeping and settlement remain unchanged;
- unrelated 5xx responses do not become paid-fallback triggers.

When the Codex timeout circuit is open and an eligible paid provider is available, later requests now continue through
Metered or Surplus instead of returning the immediate 503 seen in this incident.

## Validation

A focused regression recreates the production condition by opening the Codex upstream-timeout circuit before a
`/v1/responses` request. It verifies:

- Codex is not dispatched while the circuit is open;
- Metered is selected exactly once;
- the response is HTTP 200;
- `x-uos-upstream` is `metered`;
- terminal telemetry records `primary_upstream_degraded`.

The focused Metered paid-fallback matrix passed all 25 steps. Formatting, lint, type checking, and Git diff checks also
passed.

## Remaining risk

This repair prevents the repeated immediate-503 failure storm after the timeout circuit opens. It cannot guarantee that
the first request which detects a new upstream hang will succeed; that request can still reach the response-header
deadline. Covering the first detecting request requires a separate design decision: reserve time for an earlier paid
failover, or configure another independent Codex account/path. Either option changes latency, credential, or spending
behavior and must retain explicit cost and replay safeguards.

No evidence supports describing this incident as a global OpenAI outage.

## Evidence and production identity

- Production Git SHA during the incident: `88ed1b854fd8f1571dd39b197f77bf6436d7d851`
- Production Deno revision: `bpw68mckq1r9`
- Live Deno Deploy log pull time: 2026-08-21 11:15:37 UTC
- Preserved log capture: `logs/deno-deploy-20260821T111537Z.jsonl`
- Repair branch: `development`
- Repository: `ubiquity/ai.ubq.fi`
