# Codex reliability change audit

Reference range: `289996b6f8723ee9f5cc80cd081aa399efcc43cd..ce531ca99148f674ebec8342471d53655e5b6844`.

| Surface                               | Decision   | Rebuild contract                                                                                                                                                 |
| ------------------------------------- | ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Voyage embeddings and schemas         | Preserve   | No behavior change.                                                                                                                                              |
| Dynamic Codex catalog                 | Preserve   | Catalog remains authoritative for advertised reasoning tiers.                                                                                                    |
| OpenAI request schemas                | Preserve   | `/v1/responses`, `/v1/chat/completions`, and `/v1/models` remain compatible.                                                                                     |
| Codex `client_metadata`               | Preserve   | Accept object string maps, strip before upstream dispatch, reject malformed values.                                                                              |
| Reasoning `none` and `ultra`          | Preserve   | Keep explicit `none`; translate `ultra` to upstream `max`.                                                                                                       |
| Codex 401 refresh                     | Preserve   | One refresh-and-retry remains in the Codex client.                                                                                                               |
| metered fallback                      | Replace    | Dispatch exactly once and only after the request's live final Codex 429.                                                                                         |
| Global Codex 429 circuit              | Remove     | No cached cooldown, probe lease, inference KV read/write, or migration classification.                                                                           |
| Provider-specific SSE parsers         | Replace    | One strict, incremental parser owns all Responses terminal semantics.                                                                                            |
| Premature stream completion           | Remove     | EOF/read/malformed events fail; no invented completion or `[DONE]`.                                                                                              |
| Single paid fallback reservation slot | Replace    | Versioned V3 window, immutable request, and pending index records.                                                                                               |
| Inference-path reconciliation         | Replace    | V3 pending indexes, per-key leases, terminal triggers, and direct Deploy 2 cron scans replace legacy scans; KV queues are optional acceleration where supported. |
| Shared Codex quota headers            | Remove     | Only bounded client budgets may emit client-specific warnings.                                                                                                   |
| Admin policy fields and `-1`          | Preserve   | Existing controls remain; displayed accounting is projected from V3.                                                                                             |
| Deployment workflow                   | Replace    | Separate preview app, pinned workflow, source digest, and immutable identity headers.                                                                            |
| Production rollback reference         | Preserve   | `ce531ca` / revision `0w0c8dth0tfj` remains the rollback target.                                                                                                 |
| Deployment `x20t7k1rf0t2`             | Quarantine | Never eligible for promotion.                                                                                                                                    |

## Queue and deployment constraints

- Deno Deploy 2 does not support Deno KV queues (`Deno.Kv.enqueue()` or `Deno.Kv.listenQueue()`). Admission atomically
  writes the V3 request, pending marker, and bounded reservation; the pending marker is the durable reconciliation
  source.
- Production `serve.ts` registers a per-minute `Deno.cron` that scans due pending markers directly. Per-key leases and
  idempotent request/window commits make repeated or overlapping scans safe.
- Local Deno and Deploy Classic may use KV queue delivery as optional acceleration for terminal reconciliation. Queue
  failures never roll back durable admission or backoff state; the Deploy 2 cron scan remains the recovery path.
- Terminal events expedite a pending marker, while provider-log reads and settlement stay off the inference request
  path.
- Acceptance verifies both the exact returned revision URL and `p-ai-ubq-fi.ubiquity-dao.deno.net`; both must identify
  the same isolated revision before production promotion.
