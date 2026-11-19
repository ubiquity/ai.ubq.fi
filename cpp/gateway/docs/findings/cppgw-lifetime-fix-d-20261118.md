# Native gateway lifetime fixes, area D

## Contract v1

Fix or reclassify all 29 findings in the [area D lifetime audit](cppgw-lifetime-d-20261118.md) against the
integrated source at `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The audit-only merge is
`dc3abc2f904b8d93a8053840c05b772938841beb`. The first source unit addresses the six private race losers, followed by
startup shutdown ordering, the OpenRouter index, callback ownership, cycles and await expressions.

| Surface | Required behavior |
| --- | --- |
| Body reads, Responses, telemetry, provisioning, replay and health | Preserve the deciding result, deadlines and late operation observation. Dispose of synthetic losers once the race settles. |
| Startup and catalog | Server startup can overlap migration. Finish the migration before KV closure. Publish each catalog snapshot with its own index. |
| Admin, auth and terminal hooks | Preserve callback inputs, failure types, rejection timing, settlement, request drain and accounting. A pending callback must own its callable. |
| Await expressions and release | Follow the GCC 12.2 comparison rules. Test the supported Linux ARM64 build and publish the scoped change for review. |

The owned surface is the direct `cpp/gateway/src/` files, the assigned handler, admin, auth, authority, Sentinel,
models, catalog, lib, compaction, conversion, systemone and decisions trees and their headers, affected gateway
tests and this report. The other C++ areas, TypeScript, workflows, credentials, live provider traffic, deployment
and changes to shared runtime Promise semantics are outside scope. Wire output, storage, quota and accounting
behavior are invariants. Tests use a fresh home and temporary directory, a credential-free environment and the
portable driver's isolated loopback fixture network.

Before each source unit, resolve the current link closures for the changed files and run their release and sanitizer
gates. Final acceptance covers four affected sanitizer shards, one complete release shard,
`python3 cpp/gateway/tools/links.py --check` and `git diff --check`. The driver builds all production objects on a
cold run. The integrated runner measured about 46 minutes for cold release and 69 minutes for cold sanitizers.
Expect incremental runs to rebuild the changed objects and run the affected tests. Build only in external scratch,
use up to four workers across the suite builds, and keep the driver's incremental results and terminal receipts. If a
gate fails, retain the diagnostic and rerun only the affected gate after a fix. Stop when every row has an evidenced
verdict and the selected gates pass, or commit the building subset and record any outstanding work.

The current unknowns are which claims still hold after integration, the availability of the toolchain and pinned
libraries, GitHub write access and the cost of cold verification. Dependency setup is bounded to 20 minutes before
reassessment. Report rows use the first line number from each audit ledger entry as a stable identifier. The commit
column identifies a source commit by its unique subject so the report can travel in that same commit.

## Defect ledger

| Audit ID | Verdict | Commit | Test |
| --- | --- | --- | --- |
| `src/bounded_response_body.cpp:41` | FIXED · `abortableRead` settles its private abort latch after unregistering the listener, releasing each completed read result. | Settle private race losers | `bounded_response_body_test` measures retained 64 KiB chunks on a live loop. |
| `src/responses_stream.cpp:287` | FIXED · `readWithDeadline` settles its private inactivity latch after unregistering the timeout listener. | Settle private race losers | `responses_stream_test` reads text, a 64 KiB comment and a terminal event before sampling allocations. |
| `include/uos/optional_telemetry_queue.hpp:258` | FIXED · `waitForProgress` finishes the private timeout promise as it clears the timer. | Settle private race losers | `optional_telemetry_queue_differential_test` repeats successful writer waits and checks the existing timing traces. |
| `src/auth/local_development_key.cpp:97` | FIXED · `awaitWithAbort` settles the observed private abort promise after detaching it. The pricing work stays observed if the abort wins. | Settle private race losers | `local_development_key_test` provisions fresh in-memory stores and measures retained allocations. |
| `src/sentinel/replay_retention_bootstrap.cpp:469` | FIXED · `settledWithin` resolves the private wait after clearing its timer and before delivering the original result. | Settle private race losers | `sentinel_replay_retention_test` repeats completed owner fills against fresh in-memory stores. |
| `src/health.cpp:306` | FIXED · `probeUpstreamCoalesced` settles the private timeout promise when clearing the timer and the coalescing slot. | Settle private race losers | `health_test` checks one configured mock probe and measures the health timer across repeated unconfigured probes. |
| `src/serve.cpp:123` | FIXED · Both launchers retain the migration. Normal shutdown and prepared startup failure await it before telemetry and KV close. | Drain startup migration before KV closure | `serve_test` delays the migration read and forces the loopback listener setup callback to fail, then checks close ordering and the original error. |
| `src/models/openrouter_models.cpp:158` | FIXED · `buildMatchIndex` takes raw row pointers. Complete the index before replacing the cache or index, so the failed-refresh fallback keeps both old owners. | Publish catalog and index together | `openrouter_models_test` injects an allocation failure at the index build and checks the prior owner's lifetime, lookups and a successful retry. |
| `src/admin/codex.cpp:207` | FIXED · The whitelist read can reject while the catalog override is suspended. The child awaits with the invoked callable owned. | Retain invoked callbacks through settlement | `admin_codex_coverage_test` rejects the whitelist read before resuming a captured callback. |
| `src/admin/codex.cpp:216` | FIXED · Any refresh child can reject while an override stays suspended. The five override tasks retain their callable and receive value options. | Retain invoked callbacks through settlement | `admin_codex_coverage_test` rejects Surplus before releasing a pending OpenLux override. |
| `src/admin/codex.cpp:238` | FIXED · Provider selection uses the uncaught strong-read error path, so it can reject the join. The catalog child now retains its callable. | Retain invoked callbacks through settlement | `admin_codex_coverage_test` rejects the selection read and resumes the pending catalog override. |
| `src/admin/defaults.cpp:53` | FIXED · A rejected Kernel limit read can leave the diagnostics override pending. Its Task now holds the callable. | Retain invoked callbacks through settlement | `admin_defaults_differential_test` rejects the limit read first and resumes diagnostics afterwards. |
| `src/handler/terminal_log.cpp:61` | FIXED · The first writer can suspend before a later factory throws. Each injected writer's original promise now holds the callable until settlement. | Retain invoked callbacks through settlement | `prompt_cache_telemetry_gate_test` throws from the second writer and checks that the first resumes after the response. |
| `src/handler/terminal_route.cpp:523` | FIXED · Background registration permits the caller to leave while the injected persistence Task is still suspended. The existing completion chain keeps its callable. | Retain invoked callbacks through settlement | `handler_replay_background_test` resumes the callback after its scheduling coroutine has returned. |
| `src/handler/terminal_log.cpp:519` | LEFT · `sendResponse` prepares native headers before taking the body. A header rejection can deliver a fallback 500 without settling the original SSE body. | Transport owner | The cancellation belongs to `src/runtime/http.cpp`, which is outside this branch's assigned paths. |
| `src/inference_admission.cpp:170` | LEFT · Pending re-verification. | | |
| `src/kv.cpp:56` | FIXED · `setKvForTest` can replace the selected opener while its Task is suspended. The awaiting open frame now owns the exact invoked copy. | Retain invoked callbacks through settlement | `storage_bun_kv_test` replaces the opener, resumes the earlier attempt and checks the returned store. |
| `src/jev_compaction/compaction.cpp:238` | LEFT · Pending re-verification. | | |
| `src/lib/jev_compaction/jev_codex_smoke.cpp:180` | LEFT · Pending re-verification. | | |
| `src/serve.cpp:159` | FIXED · `process::once` stores its callback before arming the signal wait. The development launcher's cleanup guard now covers both registrations. | Drain startup migration before KV closure | `serve_test` covers startup cleanup. Faulting a valid OS signal registration requires intrusive failure injection. |
| `src/handler/serve_handler.cpp:13` | LEFT · Pending re-verification. | | |
| `src/admin/defaults.cpp:99` | LEFT · Pending re-verification. | | |
| `src/auth/passkeys.cpp:4313` | LEFT · Pending re-verification. | | |
| `src/admin/codex.cpp:71` | LEFT · Pending re-verification. | | |
| `src/admin/kernel.cpp:380` | LEFT · Pending re-verification. | | |
| `src/auth/index.cpp:137` | LEFT · Pending re-verification. | | |
| `src/models/codex_models_whitelist.cpp:134` | LEFT · Pending re-verification. | | |
| `src/responses_failover_stream.cpp:944` | LEFT · Pending re-verification. | | |
| `include/uos/responses_stream.hpp:125` | LEFT · Pending re-verification. | | |

## Verification and handoff

The audit records zero UNCLEAR rows. In the integrated `rt::race` implementation, each pending source still owns
a reaction that holds the combined result. Cleanup at all six sites removed the callback able to settle a private
source. The new tests sample ASan's live allocation count before the loop is destroyed. The loop's teardown
otherwise removes pending reactions and can leave LeakSanitizer clean even after a long-running process retains
memory. The checked behavior includes the first event, inactivity, timeout and abort results and the existing
microtask traces. The six race changes affect 218 of 450 link closures. Strict Clang 21.1.8 syntax checks passed
for the 12 changed or instantiating source and test translation units. Release and sanitizer link and run gates
are pending.

Startup and catalog changes passed strict syntax checks for their two source and two test translation units.
Their link closures contain 3 and 129 binaries respectively. Their release and sanitizer runs are pending.
