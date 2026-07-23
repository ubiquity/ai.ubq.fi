# Codex reliability change audit

Reference range: `289996b6f8723ee9f5cc80cd081aa399efcc43cd..ce531ca99148f674ebec8342471d53655e5b6844`.

| Surface                               | Decision   | Rebuild contract                                                                       |
| ------------------------------------- | ---------- | -------------------------------------------------------------------------------------- |
| Voyage embeddings and schemas         | Preserve   | No behavior change.                                                                    |
| Dynamic Codex catalog                 | Preserve   | Catalog remains authoritative for advertised reasoning tiers.                          |
| OpenAI request schemas                | Preserve   | `/v1/responses`, `/v1/chat/completions`, and `/v1/models` remain compatible.           |
| Codex `client_metadata`               | Preserve   | Accept object string maps, strip before upstream dispatch, reject malformed values.    |
| Reasoning `none` and `ultra`          | Preserve   | Keep explicit `none`; translate `ultra` to upstream `max`.                             |
| Codex 401 refresh                     | Preserve   | One refresh-and-retry remains in the Codex client.                                     |
| Yunwu fallback                        | Replace    | Dispatch exactly once and only after the request's live final Codex 429.               |
| Global Codex 429 circuit              | Remove     | No cached cooldown, probe lease, inference KV read/write, or migration classification. |
| Provider-specific SSE parsers         | Replace    | One strict, incremental parser owns all Responses terminal semantics.                  |
| Premature stream completion           | Remove     | EOF/read/malformed events fail; no invented completion or `[DONE]`.                    |
| Single paid fallback reservation slot | Replace    | Versioned V3 window, immutable request, and pending index records.                     |
| Inference-path reconciliation         | Remove     | Durable reconciliation is queue-driven.                                                |
| Shared Codex quota headers            | Remove     | Only bounded client budgets may emit client-specific warnings.                         |
| Admin policy fields and `-1`          | Preserve   | Existing controls remain; displayed accounting is projected from V3.                   |
| Deployment workflow                   | Replace    | Separate preview app, pinned reusable workflow, immutable identity headers.            |
| Production rollback reference         | Preserve   | `ce531ca` / revision `0w0c8dth0tfj` remains the rollback target.                       |
| Deployment `x20t7k1rf0t2`             | Quarantine | Never eligible for promotion.                                                          |
