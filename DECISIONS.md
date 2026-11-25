# Gateway decisions

## 2026-11-25 — Concurrent inference streams

Each gateway process admits up to 512 active inference requests so a burst of 500 coding agents can each hold a stream. A further 128 requests can wait up to five seconds for a permit. The gateway returns HTTP 503 before contacting a provider when that queue fills or a wait expires.
