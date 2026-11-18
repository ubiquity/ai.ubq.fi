# Native gateway lifetime fixes, Area C

## Contract v1

Reverify the 23 DEFECT rows in the Area C audit against the integrated gateway, then repair
confirmed lifetime defects at their existing entry points. Prioritize the Codex retry, abort
listeners and fail-fast request reads. The audit merged at `78b5276cad1ad07ae0e037cb1354cccb28352bec`
without a production source change. The assigned integrated parent is
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. [Area C audit](cppgw-lifetime-c-20261118.md)

| Request surface | Invariants |
| --------------- | ---------- |
| Codex, provider and paid fallback | Keep the selected account/provider, deadline and retry behavior, HTTP statuses and error reasons. Preserve delivery receipts and one accounting settlement per admission. |
| Embeddings and KV | Preserve keys, values, consistency levels, fail-fast rejection and independent completion of started reads. |
| Native supervisor, Claude and provider discovery | Preserve abort and timeout behavior, shared work, request coalescing and cache generations. |
| DeepSeek, Harmony, chat and live | Preserve wire and stream behavior. The audit assigns no defect rows to these modules. |

Write only Area C source and headers, their tests and this report. Shared runtime, other C++ areas,
TypeScript, workflows, deployment and credentials are outside this work. Tests use local mocks
behind the CI driver's OS fixture boundary, with an empty home, fresh temporary directories and
a clean environment. No provider traffic. Dependencies and build outputs live outside the checkout.

Acceptance is one building commit per defect or family with a report row, a deterministic
regression where possible, the affected link targets in release and ASan after each family, all
four affected ASan shards, one full release shard, and
`python3 cpp/gateway/tools/links.py --check`. The exact failing interleavings and achievable
test coverage are discovery questions. Stop after those gates and the report reconcile, or
checkpoint the building subset and mark outstanding work LEFT if interrupted.

Dependency preparation uses the pinned upstream Boost, SQLite and ICU sources, Clang 21 and
the Ubuntu development packages. Budget 20 minutes for dependency preparation and 90 minutes
per cold four-worker CI configuration. The logs and driver receipts are under
`/var/tmp/cppgw-lifetime-fix-c-20261118`. Check compiler and test phase transitions and diagnose
a failed component through its log before retrying. Bound individual test executions through
the driver. Use incremental builds for the defect checkpoints.

## Defect ledger

IDs include the original audit line range. Each LEFT entry initially records pending verification.

| ID | Verdict and evidence | Commit | Test |
| -- | -------------------- | ------ | ---- |
| dispatch.cpp:628–635 | LEFT — retry sleeper closure pending verification. | — | — |
| dispatch.cpp:623–635 | LEFT — retry abort listener pending verification. | — | — |
| auth_refresh.cpp:535–548 | LEFT — shared refresh abort listener pending verification. | — | — |
| supervisor_transport.cpp:134–140 | LEFT — per-call listener pending verification. | — | — |
| voyage.cpp:198–207 | LEFT — embeddings retry listener pending verification. | — | — |
| claude_code.cpp:100–119 | LEFT — Claude abort listener pending verification. | — | — |
| metered.cpp:193–230 | LEFT — Metered abort race pending verification. | — | — |
| surplus.cpp:318–354 | LEFT — Surplus preheader abort race pending verification. | — | — |
| manual_reset.cpp:88,161–162,199–201 | LEFT — manual-reset KV reads pending verification. | — | — |
| reset_settings.cpp:59–67,108–112 | LEFT — reset-settings KV reads pending verification. | — | — |
| handlers.cpp:137–140 (embeddings) | LEFT — cache lookup KV reads pending verification. | — | — |
| ledger.cpp:161–168 (embeddings) | LEFT — replay KV reads pending verification. | — | — |
| voyage.cpp:126–136 | LEFT — eviction KV reads pending verification. | — | — |
| ledger_backfill.cpp:451–457 | LEFT — paid backfill KV reads pending verification. | — | — |
| ledger_settlement.cpp:266–271,322–327 | LEFT — deferral and settlement KV reads pending verification. | — | — |
| health.cpp:254–266 | LEFT — health KV reads pending verification. | — | — |
| health.cpp:158–172 | LEFT — health transition clock reentry pending verification. | — | — |
| metered.cpp:684–707 | LEFT — Metered abandoned discovery pending verification. | — | — |
| selection.cpp:82–98,264–277 | LEFT — abandoned selection load pending verification. | — | — |
| surplus.cpp:675–698 | LEFT — Surplus abandoned discovery pending verification. | — | — |
| anthropic_chat_responses.cpp:519 | LEFT — Chat bounded read await pending verification. | — | — |
| anthropic_responses.cpp:779,857,880 | LEFT — Messages error and bounded reads pending verification. | — | — |
| claude_code_usage_refresh.cpp:35,75 | LEFT — usage refresh read options pending verification. | — | — |

The audit recorded zero UNCLEAR rows. Its reported injected opener lifetime in `src/kv.cpp`
is outside Area C and remains a separate item for the owning agent.

## Verification

The integrated base documents 450 green ARM64 test binaries. This task has no local test results yet.

## Summary

The ranked audit is merged. All 23 rows are tracked and awaiting re-verification.
