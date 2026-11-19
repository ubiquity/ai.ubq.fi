# Native gateway lifetime fixes, Area C

## Contract v1

Reverify the 23 DEFECT rows in the Area C audit against the integrated gateway, then repair
confirmed lifetime defects at their existing entry points. Prioritize the Codex retry, abort
listeners and fail-fast request reads. The audit merged at `78b5276cad1ad07ae0e037cb1354cccb28352bec`
without a production source change. The assigned integrated parent is
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. [Area C audit](cppgw-lifetime-c-20261118.md)

| Request surface                                  | Invariants                                                                                                                                                                |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Codex, provider and paid fallback                | Keep the selected account/provider, deadline and retry behavior, HTTP statuses and error reasons. Preserve delivery receipts and one accounting settlement per admission. |
| Embeddings and KV                                | Preserve keys, values, consistency levels, fail-fast rejection and independent completion of started reads.                                                               |
| Native supervisor, Claude and provider discovery | Preserve abort and timeout behavior, shared work, request coalescing and cache generations.                                                                               |
| DeepSeek, Harmony, chat and live                 | Preserve wire and stream behavior. The audit assigns no defect rows to these modules.                                                                                     |

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

| ID                                    | Verdict and evidence                                                                                                                                                                                                | Commit                                                       | Test                                                                                                                                        |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------- |
| dispatch.cpp:628–635                  | FIXED — abort can destroy the injected sleeper's closure before the losing Task resumes. The default delay is an owning free coroutine. Start the same callable synchronously from the owner retained by the child. | `fix(cpp): retain the Codex retry sleeper through abort`     | `codex_dispatch_differential_test.cpp` resumes the sleeper after the original abort rejection. Gates pending.                               |
| dispatch.cpp:623–635                  | FIXED — tearing down a suspended frame skipped removal of the listener that owned the signal. Remove the listener at scope exit, including frame destruction.                                                       | Same retry commit                                            | `codex_dispatch_differential_test.cpp` destroys a Loop with both branches pending. Gates pending.                                           |
| auth_refresh.cpp:535–548              | FIXED — the shared promise finalizer is dropped on teardown while the listener retains its own signal. The listener now locks a weak signal; the shared work and its finalizer are unchanged.                       | `fix(cpp): break abort listener ownership cycles`            | `codex_dispatch_differential_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                              |
| supervisor_transport.cpp:134–140      | FIXED — pending cleanup owns the callback which owned its own connection. The callback locks a weak connection while the call and timer retain live state.                                                          | `fix(cpp): break abort listener ownership cycles`            | `codex_supervisor_inventory_coverage_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                      |
| voyage.cpp:198–207                    | FIXED — timer teardown drops the only forward path to the wait while a once listener owned its registering signal. Only the timer retains the signal. The dispatcher removes the once listener before invoking it.  | `fix(cpp): break abort listener ownership cycles`            | `embeddings_coverage_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                      |
| claude_code.cpp:100–119               | FIXED — abandoning the racing frame discarded its on-settlement disposer. The frame now unregisters at scope exit, including teardown.                                                                              | `fix(cpp): break abort listener ownership cycles`            | `claude_code_differential_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                 |
| metered.cpp:193–230                   | FIXED — billing or catalog transport can be abandoned with a signal owning itself through the listener. The reaction-owned race state now owns the signal, and its destructor unregisters from live source signals. | `fix(cpp): break abort listener ownership cycles`            | `metered_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                                  |
| surplus.cpp:318–354                   | FIXED — preheader abandonment left the same listener and signal cycle. The reaction owns the race state and the listener only locks it weakly.                                                                      | `fix(cpp): break abort listener ownership cycles`            | `surplus_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                                  |
| manual_reset.cpp:88,161–162,199–201   | FIXED — the public override can transfer the last store owner into a reset. Both the initial snapshot and promotion use the now-owning readEntry.                                                                   | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, manual reset. Gates pending.                                                                        |
| reset_settings.cpp:59–67,108–112      | FIXED — both migration paths start virtual reads that can outlive a rejected sibling. Each read keeps the store and strong consistency.                                                                             | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, reset opt-out migration. Gates pending.                                                             |
| handlers.cpp:137–140 (embeddings)     | FIXED — the buffered embeddings handler can unwind while the second cache read is still pending. Each started cache read now retains the store.                                                                     | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache lookup. Gates pending.                                                             |
| ledger.cpp:161–168 (embeddings)       | FIXED — a rejected chunk can return the existing 503 before a second chunk resumes. Each chunk read retains the lease store.                                                                                        | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, embeddings replay. Gates pending.                                                                   |
| voyage.cpp:126–136                    | FIXED — a quota-triggered eviction may fail with another index read outstanding. The writer takes a store owner and each eviction read keeps its own copy.                                                          | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache eviction. Gates pending.                                                           |
| ledger_backfill.cpp:451–457           | FIXED — a failed state read can return before either cursor get completes. The state and cursor tasks each keep the passed store.                                                                                   | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, paid backfill. Gates pending.                                                                       |
| ledger_settlement.cpp:266–271,322–327 | FIXED — both batches could reject while an injected sibling still used its store. Each request, pending and settlement gate read keeps an owner.                                                                    | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, paid deferral and settlement. Gates pending.                                                        |
| health.cpp:254–266                    | FIXED — replacing the global store after a failed health read could free the old receiver before a sibling finished. Read frames now retain the old store independently.                                            | `fix(cpp): retain KV receivers across fail-fast reads`       | `provider_kv_fanout_lifetime_test.cpp`, provider health. Gates pending.                                                                     |
| health.cpp:158–172                    | FIXED — the injected clock can reset the queue while an earlier transition is pending. Find the current tail after the clock call. The default clock does not reset the queue.                                      | `fix(cpp): refresh health transition lookup after callbacks` | `provider_selection_health_differential_test.cpp` clears the pending queue from the clock and verifies both records persist. Gates pending. |
| metered.cpp:684–707                   | LEFT — Metered abandoned discovery pending verification.                                                                                                                                                            | —                                                            | —                                                                                                                                           |
| selection.cpp:82–98,264–277           | LEFT — abandoned selection load pending verification.                                                                                                                                                               | —                                                            | —                                                                                                                                           |
| surplus.cpp:675–698                   | LEFT — Surplus abandoned discovery pending verification.                                                                                                                                                            | —                                                            | —                                                                                                                                           |
| anthropic_chat_responses.cpp:519      | LEFT — Chat bounded read await pending verification.                                                                                                                                                                | —                                                            | —                                                                                                                                           |
| anthropic_responses.cpp:779,857,880   | LEFT — Messages error and bounded reads pending verification.                                                                                                                                                       | —                                                            | —                                                                                                                                           |
| claude_code_usage_refresh.cpp:35,75   | LEFT — usage refresh read options pending verification.                                                                                                                                                             | —                                                            | —                                                                                                                                           |

The audit recorded zero UNCLEAR rows. Its reported injected opener lifetime in `src/kv.cpp`
is outside Area C and remains a separate item for the owning agent.

## Verification

The integrated base documents 450 green ARM64 test binaries. This task has no local test results yet.

## Summary

The retry, abort-listener and fail-fast KV receiver defects are fixed across 16 audit
rows. The production KV adapters already own their backing store. The fan-out repair
also protects injected coroutine implementations while preserving immediate rejection
and allowing pending sibling reads to finish. Seven audit rows and runtime gates remain.
