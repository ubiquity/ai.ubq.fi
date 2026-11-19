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

IDs include the original audit line ranges. The verification section identifies which gates ran against each commit.

| ID                                    | Verdict and evidence                                                                                                                                                                                                | Commit                                                                | Test                                                                                                                                                                              |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| dispatch.cpp:628–635                  | FIXED — abort can destroy the injected sleeper's closure before the losing Task resumes. The default delay is an owning free coroutine. Start the same callable synchronously from the owner retained by the child. | `fix(cpp): retain the Codex retry sleeper through abort`              | `codex_dispatch_differential_test.cpp` resumes the sleeper after the original abort rejection. Gates pending.                                                                     |
| dispatch.cpp:623–635                  | FIXED — tearing down a suspended frame skipped removal of the listener that owned the signal. Remove the listener at scope exit, including frame destruction.                                                       | Same retry commit                                                     | `codex_dispatch_differential_test.cpp` destroys a Loop with both branches pending. Gates pending.                                                                                 |
| auth_refresh.cpp:535–548              | FIXED — teardown drops the shared Promise finalizer. The listener holds a weak signal, and the waiter unregisters on frame destruction so a live source cannot retain a composed signal.                            | `328b0619`, `fix(cpp): unregister abandoned provider abort listeners` | `codex_dispatch_differential_test.cpp` abandons the wait with the upstream abort source still live. Gates pending.                                                                |
| supervisor_transport.cpp:134–140      | FIXED — the listener used to retain its own pending call connection. It now locks a weak connection, and connection destruction unregisters any remaining per-call listeners.                                       | `328b0619`, `fix(cpp): unregister abandoned provider abort listeners` | `codex_supervisor_inventory_coverage_test.cpp` verifies that the socket and composed signal are released while the abort source lives. Gates pending.                             |
| voyage.cpp:198–207                    | FIXED — the timer owns the listener state and its signal. The listener holds that state weakly. Destroying a pending timer unregisters even while its abort source lives.                                           | `328b0619`, `fix(cpp): unregister abandoned provider abort listeners` | `embeddings_coverage_test.cpp` verifies teardown releases the composed signal. Gates pending.                                                                                     |
| claude_code.cpp:100–119               | FIXED — abandoning the racing frame discarded its on-settlement disposer. The frame now unregisters at scope exit, including teardown.                                                                              | `fix(cpp): break abort listener ownership cycles`                     | `claude_code_differential_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                                                       |
| metered.cpp:193–230                   | FIXED — billing or catalog transport can be abandoned with a signal owning itself through the listener. The reaction-owned race state now owns the signal, and its destructor unregisters from live source signals. | `fix(cpp): break abort listener ownership cycles`                     | `metered_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                                                                        |
| surplus.cpp:318–354                   | FIXED — preheader abandonment left the same listener and signal cycle. The reaction owns the race state and the listener only locks it weakly.                                                                      | `fix(cpp): break abort listener ownership cycles`                     | `surplus_test.cpp` abandons the operation on a scoped Loop. Gates pending.                                                                                                        |
| manual_reset.cpp:88,161–162,199–201   | FIXED — the public override can transfer the last store owner into a reset. Both the initial snapshot and promotion use the now-owning readEntry.                                                                   | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, manual reset. Gates pending.                                                                                                              |
| reset_settings.cpp:59–67,108–112      | FIXED — both migration paths start virtual reads that can outlive a rejected sibling. Each read keeps the store and strong consistency.                                                                             | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, reset opt-out migration. Gates pending.                                                                                                   |
| handlers.cpp:137–140 (embeddings)     | FIXED — the buffered embeddings handler can unwind while the second cache read is still pending. Each started cache read now retains the store.                                                                     | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache lookup. Gates pending.                                                                                                   |
| ledger.cpp:161–168 (embeddings)       | FIXED — a rejected chunk can return the existing 503 before a second chunk resumes. Each chunk read retains the lease store.                                                                                        | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, embeddings replay. Gates pending.                                                                                                         |
| voyage.cpp:126–136                    | FIXED — a quota-triggered eviction may fail with another index read outstanding. The writer takes a store owner and each eviction read keeps its own copy.                                                          | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache eviction. Gates pending.                                                                                                 |
| ledger_backfill.cpp:451–457           | FIXED — a failed state read can return before either cursor get completes. The state and cursor tasks each keep the passed store.                                                                                   | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, paid backfill. Gates pending.                                                                                                             |
| ledger_settlement.cpp:266–271,322–327 | FIXED — both batches could reject while an injected sibling still used its store. Each request, pending and settlement gate read keeps an owner.                                                                    | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, paid deferral and settlement. Gates pending.                                                                                              |
| health.cpp:254–266                    | FIXED — replacing the global store after a failed health read could free the old receiver before a sibling finished. Read frames now retain the old store independently.                                            | `fix(cpp): retain KV receivers across fail-fast reads`                | `provider_kv_fanout_lifetime_test.cpp`, provider health. Gates pending.                                                                                                           |
| health.cpp:158–172                    | FIXED — the injected clock can reset the queue while an earlier transition is pending. Find the current tail after the clock call. The default clock does not reset the queue.                                      | `fix(cpp): refresh health transition lookup after callbacks`          | `provider_selection_health_differential_test.cpp` clears the pending queue from the clock and verifies both records persist. Gates pending.                                       |
| metered.cpp:684–707                   | FIXED — destroying the waiting Loop left a process-wide Promise with no producer. An identity-checked scope guard clears the slot on frame destruction.                                                             | `fix(cpp): clear abandoned provider lookups`                          | `metered_claude_cache_test.cpp` starts a new lookup on a second Loop. Gates pending.                                                                                              |
| selection.cpp:82–98,264–277           | FIXED — Loop teardown bypassed the process-wide Promise reset. The frame guard keeps the existing load token check and leaves newer saves and reads intact.                                                         | `fix(cpp): clear abandoned provider lookups`                          | `provider_selection_test.cpp` abandons a deferred store read and loads the replacement store on a second Loop. Gates pending.                                                     |
| surplus.cpp:675–698                   | FIXED — a second Loop could join a Promise abandoned on the first. An identity-checked scope guard removes only the request from its own frame.                                                                     | `fix(cpp): clear abandoned provider lookups`                          | `surplus_test.cpp` starts a new lookup on a second Loop. Gates pending.                                                                                                           |
| anthropic_chat_responses.cpp:519      | FIXED — bounded body reads passed an owning options aggregate in the await expression. Construct the Task before the await as PORTING.md requires.                                                                  | `fix(cpp): bind provider coroutine tasks before awaiting`             | `anthropic_chat_responses_test.cpp` covers the buffered route. The Clang sanitizer profile cannot establish the GCC 12.2 compiler hazard. Gates pending.                          |
| anthropic_responses.cpp:779,857,880   | FIXED — the two bounded reads and the native HTTP error normalizer passed owning options aggregates in the await expressions. Name each Task before awaiting it.                                                    | `fix(cpp): bind provider coroutine tasks before awaiting`             | `anthropic_responses_test.cpp` covers HTTP errors and buffered success. The Clang sanitizer profile cannot establish the GCC 12.2 compiler hazard. Gates pending.                 |
| claude_code_usage_refresh.cpp:35,75   | FIXED — both usage-lease reads supplied braced options inside the await expressions. The options contain only an optional enum. Name the options and Tasks to follow the port rule.                                 | `fix(cpp): bind provider coroutine tasks before awaiting`             | `claude_usage_retry_test.cpp` and `claude_capacity_analytics_test.cpp` cover claim and cleanup. There is no owning temporary here to drive a sanitizer regression. Gates pending. |

The audit recorded zero UNCLEAR rows. Its reported injected opener lifetime in `src/kv.cpp`
is outside Area C and remains a separate item for the owning agent.

## Verification

The integrated base documents 450 green ARM64 test binaries. This repair uses Ubuntu 24.04 ARM64,
Clang 21.1.8, Boost 1.92.0 static JSON and Container, SQLite 3.54.0, ICU 78.3,
OpenSSL 3, zlib, Brotli, and Zstd. The saved `/var/tmp/cppgw-lifetime-fix-c-20261118/prepare.sh`
records the setup commands. Package setup took 19.917 s, Boost 95.878 s, SQLite 61.935 s and ICU
263.635 s with overlapping builds. SQLite's fixture probe reported 3.54.0 in both configurations.

The retry fix at `f014705af507c82b3d5d48a9599366f3108e2b5a`, tree
`55ad16ee339a4860a6fc456df78a85198044ef74`, passed 158 binaries and 2,185 cases in each profile.
The release gate took 3,220.934 s including the cold build. The incremental ASan gate took
1,548.973 s. Its baseline with the new regression observed the expired signal assertion and an
ASan heap-use-after-free after the request had already returned its original abort error.
The six abort-cycle baseline binaries each failed under LSan with the new tests. The measured leaks
were 776 bytes for shared refresh, 3,680 for the supervisor call, 496 for Voyage, 848 for Claude,
920 for Metered and 920 for Surplus. The baseline diagnostics and individual driver receipts are
under `/var/tmp/cppgw-lifetime-fix-c-20261118/receipts`.

The driver scopes each checkpoint to the test files in its diff and tests whose link lists
contain a changed source. The final ASan scope uses the union of the same paths across the
repair. `ci.sh` stages an empty HOME and fresh TMPDIR, clears the inherited environment, and
selects the prepared libraries. The driver confines fixtures to private network, mount and PID
namespaces and checks the loopback and outside-route behavior before executing any tests.
Outstanding checkpoint and final shard results will be recorded here.

## Summary

All 23 audit findings have source fixes. Aborted requests release their callbacks, and a Codex
retry can finish its sleep safely after the waiting request returns. Started KV reads keep their
store alive when a sibling rejects. Provider lookups can restart after loop teardown, the health
queue survives a reentrant clock, and the Claude body and usage lookups follow the port's coroutine
rules. The remaining verification gates are in progress.
