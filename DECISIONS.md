# Gateway decisions

## 2026-11-25 — Concurrent inference streams

Each gateway process admits up to 512 active inference requests so a burst of 500 coding agents can each hold a stream. A further 128 requests can wait up to five seconds for a permit. The gateway returns HTTP 503 before contacting a provider when that queue fills or a wait expires.

## 2026-11-25 — Resource-based inference admission

The shared gateway admits new inference streams according to memory available to the process, spare file descriptors, and event-loop responsiveness. It reserves more working memory for larger request bodies. Existing streams keep their permits when pressure increases. Excess requests can wait in a bounded, five-second queue. Operators can inspect the current limiting resource on `GET /health`. This supersedes the fixed 512-stream admission decision above.
