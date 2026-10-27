# Bounded core-state migration profile

This profile supersedes full-history ingestion for the Workers Free migration lane only. It is an offline selection over one complete stopped-source capture, never a live conversion or startup action. The source capture remains untouched.

## Measured snapshot

These measurements retain the original snapshot's provenance and predate the finite Claude retention correction on 2026-10-27. They are not current row counts or a current write-budget receipt; every new capture requires a fresh manifest.

| Field                     | Value                                                              |
| ------------------------- | ------------------------------------------------------------------ |
| Source Git SHA            | `0be69ebcced135ff1d38966aa01691eddc74f1bb`                         |
| Capture SHA-256           | `af0fb90c917992346f44751ecc645289d170c88b168b0a387472a5b677159939` |
| Captured at               | `1792630570497`                                                    |
| Source rows               | `140220`                                                           |
| Selected rows             | `2317`                                                             |
| Excluded rows             | `137903`                                                           |
| Derived rows              | `5`                                                                |
| Manifest row              | `1`                                                                |
| Durable Object row writes | `4640`                                                             |
| Workers Free daily limit  | `100000`                                                           |
| Manifest SHA-256          | `0b5bb1f38fe289bc4a440e5768088f176164050c354c8ff0189b6d568a718271` |
| Selected-capture SHA-256  | `9a4ab7aaee16079917535c9bf2e04792e1cc248fdd5db2523b020246a5b959b6` |

The write count is `2 * selected_rows + derived_rows + 1`, covering one source row, one reversible mapping row, the derived indexes, and one adoption manifest. It is independent of the excluded rows.

## Retained state

| Rule                              | Rows | Purpose                                                                      |
| --------------------------------- | ---: | ---------------------------------------------------------------------------- |
| `auth_identity_state`             |    3 | passkey user, credential, and handle                                         |
| `ubq_ai_operational_state`        |   15 | API keys, Codex auth/catalog/models, Claude credential and workspace         |
| `codex_account_control_state`     |   16 | Codex routing, capacity, whitelist, reset, and overage state                 |
| `claude_account_state`            |    3 | Claude access, accounts, and quota                                           |
| `provider_capacity_current_state` |    6 | current capacity snapshot, last-available state, and current Claude capacity |
| `provider_health_current_state`   |   46 | current provider health                                                      |
| `metered_quota_current_state`     |    1 | current Metered quota state                                                  |
| `default_operational_settings`    |    2 | kernel policy defaults                                                       |
| `runtime_control_state`           |    3 | migrations, provider selection, and runtime configuration                    |
| `current_api_key_window`          |    1 | active API-key window at capture time                                        |
| `current_paid_fallback_requests`  | 1877 | current paid-fallback window requests                                        |
| `current_paid_fallback_pending`   |  344 | pending markers matching current unresolved requests                         |

The current API window is selected only when its `window_reset_at_ms` is after the capture time. The prior API window and all per-request API usage rows are excluded because the active window row is authoritative for the current counter.

The current paid-fallback window has no window aggregate in this source. Its 1,877 request rows and 344 matching pending markers are therefore the unavoidable minimum for reconstructing current settled, reserved, and pending accounting. Adoption derives that current window aggregate from those rows inside the same transaction, so the admin reader sees the current totals. The derived window is not a reset or a historical backfill. The source reconciliation gate is also excluded and re-derived from the retained pending set because source gate versions use different document shapes.

## Excluded state

| Rule                                |   Rows | Reason                                                                                 |
| ----------------------------------- | -----: | -------------------------------------------------------------------------------------- |
| `historical_api_key_usage_requests` | 105257 | per-request usage history; current window retained                                     |
| `historical_observability`          |  18681 | Sentinel replay/incident and prompt-cache analytics/telemetry                          |
| `historical_paid_fallback`          |  10626 | old paid requests, rollups, stats, leases, and backfill state; current window retained |
| `historical_provider_capacity`      |   1033 | capacity history, rollups, downtime, and reset events; current snapshot retained       |
| `historical_metered_balance`        |    916 | balance history; current state retained                                                |
| `historical_api_key_audit`          |      2 | API-key change audit                                                                   |
| `historical_error_log`              |   1386 | `admin_error_log`                                                                      |
| `historical_api_key_window`         |      1 | previous API window                                                                    |
| `recomputed_paid_fallback_gate`     |      1 | derived from retained current pending rows                                             |

The five owner-named families `admin_error_log`, `sentinel_replay`, `sentinel_incident`, `prompt_cache_analytics`, and `prompt_cache_telemetry_gate` are all excluded.

## Quota consequence

The active API-key counter is preserved exactly. Historical API request rows are not required because the active window row carries `committed_requests` and `reserved_requests`.

The current paid-fallback counter is preserved as request and pending rows and one derived current-window aggregate. The source did not provide that aggregate; the migration computes it from the retained current requests and pending markers. The Rust admin reader therefore sees the current settled, reserved, and pending totals. Inverse export drops the derived aggregate because it is not source state, while the underlying current request and pending rows remain available.

No live quota counter is reset, rounded, or synthesized from historical logs.

## Acceptance boundary

The current selector retains the exact `["uos_ai","claude_code_usage","v1","lease",account_id,account_cohort_id]` collector row as source DATA, together with full current Claude quota rows and their `usage` and `usage_snapshot` fields. It preserves each retained key, typed value, revision and captured absolute expiry. A lease owner never becomes collector, authentication or dispatch authority, and migration never recomputes or renews its expiry.

Only the proven Claude `provider_capacity/claude/v1/history`, `rollup` and `usage_rollup` key shapes are excluded from the reduced capture as historical observability. Their source TTLs remain untouched; hourly rollups have no TTL. Unknown Claude families, versions and key shapes fail closed. Current quota windows, proofs and snapshots remain operational state. Full-history export retains its existing behavior; a reduced latest inverse describes the selected subset and its subsequent writes/deletions, not a reconstruction of excluded history.

The synthetic source fixture is produced by `tests/support/claude-retention-source.mjs` through the pinned TypeScript quota, usage-refresh and history writers with a fixed CAS fixture clock, an injected GET response and a fake credential. Its expiry metadata records that synthetic commit clock plus the actual source `expireIn`; it does not claim a native KV expiry measurement. The fixture is captured during the post-quota history callback before lease cleanup, keeping a coherent lease/current-quota/all-three-history state. The Rust lifecycle fixture exercises selection, actual SQLite restart, later current-row writes and lease deletion, latest inverse and re-adoption without changing retained expiry or counters.

The focused tests cover selection accounting, atomic adoption, staged spool adoption, restart, API-key authentication, admin key reads, quota reservation, current paid pending/accounting rows, latest native inverse, and re-adoption. They do not authorize live adoption, credential refresh, Cloudflare deployment, or production cutover.

The full snapshot still requires current native custody for the raw `ubq_ai/codex_auth` refresh material before preparation. The captured generation does not match the current local auth files; no credential refresh or rotation is performed by this profile.
