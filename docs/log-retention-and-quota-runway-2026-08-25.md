# Log retention tiers and quota-runway projection — 2026-08-25

## Goal

Keep inference history as long as possible for research without unbounded raw-log growth, and let operators estimate how
long a run (for example `gpt-5.6-sol` or `gpt-5.6-luna`) can last before the paid quota balance runs out.

## Retention decision

Paid-fallback request rows used to be retained indefinitely in Deno KV. That is now bounded at one year, and
research-grade history lives in two compact stores:

| Store                         | Key prefix                                                       | Retention                     | Contents                                                                                                                                                                                                     |
| ----------------------------- | ---------------------------------------------------------------- | ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Paid-fallback raw rows        | `uos_ai/paid_fallback/v3/request/...`                            | 365 days from `created_at_ms` | Every request row (admission, dispatch, settlement, terminal)                                                                                                                                                |
| Model usage rollups           | `uos_ai/paid_fallback/v3/usage_rollup/<hour>/<model>/<provider>` | Indefinite                    | Per-hour per-model per-provider sums: requests, quota, tokens, spend. Settled paid traffic plus one terminal observation per response on every route that never settles (Codex subscription capacity first). |
| Metered quota balance history | `uos_ai/metered_quota/v1/balance_history/<hour>`                 | Indefinite                    | Hourly wallet balance / baseline / remaining percent (+ totals in token-usage mode)                                                                                                                          |
| Provider capacity history     | `uos_ai/provider_capacity/v1/history/...`                        | 7 days (unchanged)            | 15-minute Codex/Metered capacity snapshot used by the admin chart                                                                                                                                            |
| Admin error log               | `uos_ai/admin_error_log/v1/...`                                  | 7 days (unchanged)            | Failed inference terminals                                                                                                                                                                                   |
| Prompt-cache analytics        | `uos_ai/prompt-cache-analytics/...`                              | 8 days (unchanged)            | Cache-token buckets                                                                                                                                                                                          |

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

The rollup is also written by `recordTerminalUsageRollup` (src/paid_fallback_rollups.ts) from the terminal request log
in `src/handler.ts`, once per terminal response, for every route the settlement cannot see — the Codex subscription
capacity first, plus the other direct providers. That observation carries the model, the provider route, the observed
token counts and the request hour, never an upstream request id, and writes zero quota and spend so the paid balance
math is never inflated. Providers the settlement already covers (`metered`, `surplus`) are skipped, as are `gateway`
rejections and `mixed` image-fanout aggregates, so every request is counted exactly once. The write is bounded and best
effort: one strong read plus one atomic merge, up to three attempts on a lost compare-and-set race, and a KV failure
never changes an already-terminal response.

### Measured KV cost of the terminal observation

`tests/usage-optimization-measurement.test.ts` records the exact per-scenario KV budget, and it pins the added cost
instead of absorbing it. One terminal response that reaches the writer costs exactly one strong read and one
compare-and-set commit; only a lost race retries the merge, bounded at three attempts. Settled providers, `gateway` and
`mixed` labels, and responses with no observed model never reach a KV call at all.

| Fixture scenario                    | Reads before → after | Writes before → after | Commits before → after |
| ----------------------------------- | -------------------: | --------------------: | ---------------------: |
| `bounded_api_key:client_disconnect` |              18 → 19 |                 6 → 7 |                  3 → 4 |
| `bounded_api_key:success`           |              19 → 20 |                 8 → 9 |                  4 → 5 |
| `bounded_api_key:upstream_failure`  |              28 → 29 |               18 → 19 |                10 → 11 |
| `unlimited_api_key:success`         |              18 → 19 |                 8 → 9 |                  4 → 5 |
| `uos_allowlist:success`             |              12 → 13 |                 4 → 5 |                  2 → 3 |
| `admin_allowlist:success`           |              12 → 13 |                 4 → 5 |                  2 → 3 |
| `codex_auth_pool:retry`             |              13 → 13 |                 4 → 4 |                  2 → 2 |

`codex_auth_pool:retry` calls the Codex transport directly, so it produces no terminal response and no observation.
`bounded_api_key:concurrent_admission` also gains the winning response's single observation, but its totals vary with
task scheduling. Against the audit profile of about 14.25 reads and 2.83 writes per request
(`docs/deno-free-tier-audit-2026-08-09.md`), the added read and write are about +7% reads and +35% writes for the
current Codex-subscription-dominated request mix; the fixture is the authoritative before/after record.

## Quota-runway projection

`GET /admin/providers/quota-projection?window_days=7|30|90` (admin auth, default 30) returns:

- `window_days` — the requested consumption window; the rollup scan is bounded to it so the 30-second admin poll never
  pulls the full 90-day history.
- `quota` — normalized Metered quota view (wallet balance, baseline, remaining percent, totals in token-usage mode,
  refill facts).
- `models[]` — per model-provider, for the requested window: request count, quota sum, average quota per request, quota
  per hour, token and spend sums, plus `quota_source` (only `metered` is monitored) and `usage_source` (`paid_fallback`
  for settlement rows that carry quota/spend and runway estimates, `observability` for terminal accounting on routes
  that never settle, Codex subscription capacity first).
- `estimates[]` — for the requested window: requests remaining, run-time remaining, estimated exhaustion timestamp,
  percent-of-balance / percent-of-baseline knocked per request, and `stale_balance` when the quota snapshot is stale.
  Surplus rows always get an empty estimates array: `METERED_API_KEY` monitors only the OpenLux account, so projecting
  Surplus history against it would be wrong. `observability` rows never carry estimates either: they report consumption
  for a capacity the OpenLux balance does not measure.
- `balance_history` — trailing seven days of hourly balance samples.

Token-usage mode treats `total_available` as the remaining inventory (the gateway UI labels it "Available tokens"); it
is used directly and never has `total_used` subtracted from it. A stale quota snapshot (`cache_state: "stale"`) still
computes estimates but flags them for the UI.

`POST /admin/providers/quota-projection/backfill?limit=N` folds already-settled V3 rows (which predate this feature and
never passed through the settlement hook) into rollups and applies the anchored raw-row TTL to pre-existing rows. It is
idempotent and resumable: rows carry `usage_rollup_at_ms` set by the settlement write for live traffic and by this
backfill for historical rows, so a run can never double-count usage already folded into a rollup. The `limit` budgets
rows needing work, not already-backfilled ones. Repeat until `truncated` is false.

The balance samples, rollups and request rows are registered in `kv_migration.ts` `DURABLE_PREFIXES` so KV export and
import preserve them. Window rows expire one year after their reset (matching the raw-row horizon), and migration
validation skips window consistency checks beyond that horizon instead of reporting aged-out history as corrupt. The
backfill also rewrites the window prefix with the anchored TTL for rows that predate it.

The Metered balance history is namespaced by a non-secret fingerprint of the configured OpenLux credentials, so rotating
`METERED_API_KEY` starts a fresh curve for the new account instead of mixing accounts in one run-down series.

Rollup keys are sharded by request id (16 shards) so concurrent settlements of the same model/provider never contend on
one KV key inside the settlement atomic; readers sum all shards. Windows and rates are hour-bucket precise: the selected
window includes the bucket that contains its start, and the rate divides by the full window, idle hours included.

The math is deliberately conservative: run-time is `remaining balance / quota
per hour` (from the same window), requests
remaining is `balance / average
quota per request`, and the UI states that refill is not assumed. Unlimited quota yields
no exhaustion estimate rather than a fake one.

The admin providers view renders this in a "Quota runway" card below the capacity chart (static/admin.html,
static/admin.js, static/admin.css).

## Follow-ups

- Byte/count baseline: measure actual settled-row and rollup sizes in production (see
  docs/deno-usage-optimization-plan-2026-08-09.md) before choosing a raw-row horizon; one year is the current default.
- Oldest-first hard cap: not implemented. TTL gives a bounded horizon; a true byte budget would need timestamp-ordered
  keys or a global age index — note the legacy analytics key shape `[keyId, createdAtMs, requestId]` as a precedent if
  that becomes necessary.
- The provider-capacity chart view stays at seven days on purpose; the balance history store is the long-term curve.
