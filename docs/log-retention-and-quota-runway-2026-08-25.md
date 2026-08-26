# Log retention tiers and quota-runway projection — 2026-08-25

## Goal

Keep inference history as long as possible for research without unbounded raw-log growth, and let operators estimate how
long a run (for example `gpt-5.6-sol` or `gpt-5.6-luna`) can last before the paid quota balance runs out.

## Retention decision

Paid-fallback request rows used to be retained indefinitely in Deno KV. That is now bounded at one year, and
research-grade history lives in two compact stores:

| Store                         | Key prefix                                                       | Retention                     | Contents                                                                            |
| ----------------------------- | ---------------------------------------------------------------- | ----------------------------- | ----------------------------------------------------------------------------------- |
| Paid-fallback raw rows        | `uos_ai/paid_fallback/v3/request/...`                            | 365 days from `created_at_ms` | Every request row (admission, dispatch, settlement, terminal)                       |
| Paid-fallback usage rollups   | `uos_ai/paid_fallback/v3/usage_rollup/<hour>/<model>/<provider>` | Indefinite                    | Per-hour per-model per-provider sums: requests, quota, tokens, spend                |
| Metered quota balance history | `uos_ai/metered_quota/v1/balance_history/<hour>`                 | Indefinite                    | Hourly wallet balance / baseline / remaining percent (+ totals in token-usage mode) |
| Provider capacity history     | `uos_ai/provider_capacity/v1/history/...`                        | 7 days (unchanged)            | 15-minute Codex/Metered capacity snapshot used by the admin chart                   |
| Admin error log               | `uos_ai/admin_error_log/v1/...`                                  | 7 days (unchanged)            | Failed inference terminals                                                          |
| Prompt-cache analytics        | `uos_ai/prompt-cache-analytics/...`                              | 8 days (unchanged)            | Cache-token buckets                                                                 |

Why rollups are the right "kept forever" shape: a settled raw row is roughly 800 B, so ~0.8 GiB per 1M rows (Pro plan
includes 5 GiB, then $0.75/GiB). The hourly rollups are ~25 KB per model-provider per day, so a year of history is about
10 MB per model. The rollup captures every number the research question needs (which model, which provider, how much
quota, how many tokens, how much spend) while discarding the per-request noise.

TTL mechanics: every write to a request row re-applies `created_at_ms + PAID_FALLBACK_REQUEST_LOG_RETENTION_MS` so later
lifecycle updates can never silently drop the expiry. Keyed-anchored TTL also prevents a stuck reconciliation from
keeping a row alive forever. The KV migration that copies legacy rows applies the same TTL so a migration re-run cannot
resurrect aged-out rows.

## Where the rollup is written

`settlePaidFallbackRequestV3` (src/paid_fallback_ledger.ts) is the single choke point where the authoritative
`provider_quota`, token counts and spend are read from the provider log or direct surplus settlement. The rollup merge
happens in the _same atomic_ as the settlement, so a settled request can never miss its rollup, and a replay after an
already-settled row cannot double count. The Metered quota refresh path (`getMeteredQuotaSnapshot`) appends one hourly
balance sample per refresh; at most one sample per hour bucket is kept.

## Quota-runway projection

`GET /admin/providers/quota-projection?window_days=7|30|90` (admin auth, default 30) returns:

- `window_days` — the requested consumption window; the rollup scan is bounded to it so the 30-second admin poll never
  pulls the full 90-day history.
- `quota` — normalized Metered quota view (wallet balance, baseline, remaining percent, totals in token-usage mode,
  refill facts).
- `models[]` — per model-provider, for the requested window: request count, quota sum, average quota per request, quota
  per hour, token and spend sums.
- `estimates[]` — for the requested window: requests remaining, run-time remaining, estimated exhaustion timestamp, and
  percent-of-balance / percent-of-baseline knocked per request.
- `balance_history` — trailing seven days of hourly balance samples.

The math is deliberately conservative: run-time is `remaining balance / quota
per hour` (from the same window), requests
remaining is `balance / average
quota per request`, and the UI states that refill is not assumed. In token-usage mode
the balance is `total_available - total_used`; unlimited quota yields no exhaustion estimate rather than a fake one.

The admin providers view renders this in a "Quota runway" card below the capacity chart (static/admin.html,
static/admin.js, static/admin.css).

## Follow-ups

- Byte/count baseline: measure actual settled-row and rollup sizes in production (see
  docs/deno-usage-optimization-plan-2026-08-09.md) before choosing a raw-row horizon; one year is the current default.
- Oldest-first hard cap: not implemented. TTL gives a bounded horizon; a true byte budget would need timestamp-ordered
  keys or a global age index — note the legacy analytics key shape `[keyId, createdAtMs, requestId]` as a precedent if
  that becomes necessary.
- The provider-capacity chart view stays at seven days on purpose; the balance history store is the long-term curve.
