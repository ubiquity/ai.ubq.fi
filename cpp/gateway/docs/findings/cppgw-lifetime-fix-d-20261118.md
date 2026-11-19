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
| `src/inference_admission.cpp:170` | FIXED · Pending waiters used to own the abort listener that retained both waiter and controller. The exact unreferenced timer now owns the controller externally, and teardown can release the weak listener and waiter. | Break listener and server ownership cycles | `oss_inference_admission_test` abandons a queued wait with no other owners and observes the caller signal expire at loop teardown. |
| `src/kv.cpp:56` | FIXED · `setKvForTest` can replace the selected opener while its Task is suspended. The awaiting open frame now owns the exact invoked copy. | Retain invoked callbacks through settlement | `storage_bun_kv_test` replaces the opener, resumes the earlier attempt and checks the returned store. |
| `src/jev_compaction/compaction.cpp:238` | FIXED · The wrapper could be destroyed with a buffered chunk and no further pulls. Its state destructor now clears the timer and removes the forwarding listener from the live external signal. | Break listener and server ownership cycles | `jev_compaction_test` discards buffered responses repeatedly with the same signal and samples live allocations. |
| `src/lib/jev_compaction/jev_codex_smoke.cpp:180` | FIXED · The fake owns its server handle, which previously retained the fake through the registered handler. Registration is weak and each request holds a locked owner. | Break listener and server ownership cycles | `jev_codex_smoke_test` starts and stops the loopback fake and checks that its owner expires. |
| `src/serve.cpp:159` | FIXED · `process::once` stores its callback before arming the signal wait. The development launcher's cleanup guard now covers both registrations. | Drain startup migration before KV closure | `serve_test` covers startup cleanup. Faulting a valid OS signal registration requires intrusive failure injection. |
| `src/handler/serve_handler.cpp:13` | FIXED · The request signal owned a listener that held the same signal. The handoff owns the signal and the listener now holds it weakly. | Break listener and server ownership cycles | `serve_delivery_http_test` drops a handoff without calling it and checks that the request signal is released. |
| `src/admin/defaults.cpp:99` | FIXED · `applyDefaultsUpdateAttempt` now binds the atomic result before reading `ok`. | Name owning coroutine inputs and results | Existing `admin_defaults_differential_test` commit paths. |
| `src/auth/passkeys.cpp:4313` | FIXED · All six audited lookups or deletions still constructed their keys in the await expression. Each now receives a named key. | Name owning coroutine inputs and results | Existing `passkeys_test`, `passkeys_part1_test` and `passkeys_part2_test`. |
| `src/admin/codex.cpp:71` | FIXED · The debug-routing DELETE handler names the reset scenario before awaiting. | Name owning coroutine inputs and results | Existing DELETE fixture in `admin_codex_coverage_test`. |
| `src/admin/kernel.cpp:380` | FIXED · Cohort hashing, reset and overage reads, and the overage write now use named strings. | Name owning coroutine inputs and results | Existing `admin_kernel_coverage_test`. |
| `src/auth/index.cpp:137` | FIXED · The GitHub cache input and policy queue path are built before awaiting. The digest call takes ownership of the assembled input. | Name owning coroutine inputs and results | Existing `auth_responses_branch_coverage_test`. |
| `src/models/codex_models_whitelist.cpp:134` | FIXED · The strong-read options and Task are named before awaiting. | Name owning coroutine inputs and results | Existing whitelist and failure fixtures in `admin_codex_coverage_test`. |
| `src/responses_failover_stream.cpp:944` | FIXED · Both `emitFailureTerminal` calls move their event inputs into a named Task before awaiting it. | Name owning coroutine inputs and results | Existing `responses_failover_stream_test` and `responses_failover_branch_coverage_test`. |
| `include/uos/responses_stream.hpp:125` | FIXED · `MaybeAsyncCallback::run` starts a named Task with its owned event or failure aggregate. | Name owning coroutine inputs and results | Existing `responses_stream_test` and `responses_failover_stream_test`. |

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

The callback and ownership changes affect 231 and 51 link closures respectively. The tests release their external
callback owners before resuming pending work. Ordinary admin dispatch builds empty overrides. The regression fixtures
exercise the public override entry points and the same fail-fast join. The admission timer stays exact and unreferenced
and its combined signal preserves the order of caller listeners and the caller-abort preference.

The eight named-await rows are confirmed violations of `PORTING.md`. Their results and inputs already have the expected
wire or storage shape under Clang. The existing regression fixtures exercise those behaviors, and the comparison-only
compiler failure is outside the portable Clang sanitizer profile. The await changes affect 222 link closures.

### Runner and commands

The runner is Ubuntu 24.04.5 Linux ARM64 with four CPUs. Clang and Clang++ 21.1.8 came from apt.llvm.org. The
staged official archives were SHA-256 verified before building Boost 1.92.0 static JSON and Container, ICU 78.3 and
SQLite 3.54.0 with `-O2 -DSQLITE_DQS=3`. Ubuntu supplied OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, Zstd 1.5.5,
and mold 2.30.0. The driver independently checked SQLite 3.54.0 inside the fixture namespace.

```bash
session=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  "$@"
```

The scratch `run-gate.sh` uses that environment with `instrument` empty for release and
`-gline-tables-only -fno-sanitize-recover=all` for ASan. A scratch selector imports the unmodified portable driver.
It filters the `links.all_tests()` inventory to tests whose `links.input_paths(test, links.resolve(test, registry).sources)`
intersect the changed paths. The driver still compiles all 369 production objects and runs each selected test in a
fresh network, mount and PID namespace as UID 1001. Each unit uses its own committed snapshot in the same detached
verification checkout so Ninja can reuse unchanged objects. Only changes within a unit determine its selection.

```bash
# Run from the detached verification checkout at the source commit being tested.
bash "$scratch/run-gate.sh" "$config" 2 python3 -B "$scratch/select.py" cpp/gateway changes \
  "${changed_paths[@]}" -- --config "$config"
bash "$scratch/run-gate.sh" asan 4 python3 -B "$scratch/select.py" cpp/gateway changes \
  "${all_changed_paths[@]}" -- --config asan --shard "$shard/4"
bash "$scratch/run-gate.sh" release 4 bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
python3 cpp/gateway/tools/links.py --check
git diff --check
```

Strict `clang++-21 -fsyntax-only` checks ran with the driver's warnings, C++20, sanitizer, include and native
dependency flags before the source commits. The link registry preliminary check passed all 450 entries. The
cold release and sanitizer gates are in progress, with subsequent gates pending the same checkout's release.

## Out-of-area observations

- The SSE header setup fault in the ledger reproduces by inspection in `runtime::http::sendResponse`. The transport
  owns the response at that point. It must cancel the unread body with the original failure so terminal accounting,
  admission release and the request drain can finish while preserving the fallback transport receipt.
- `src/codex/auth_refresh.cpp` still removes its private abort listener after shared auth wins without settling the
  abort promise. The Codex owner has that source path.
- `include/uos/rt/async.hpp` and `src/rt/async.cpp` still retain each pending source reaction until settlement or loop
  teardown. The runtime owner has the shared implementation. The area D changes use only local settlement.
- `src/rt/kv.cpp` still returns the awaited aggregate directly from `GenericAtomicOperation::commit`. The runtime
  owner has that source path.
