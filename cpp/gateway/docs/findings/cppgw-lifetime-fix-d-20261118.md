# Native gateway lifetime fixes, area D

## Contract v1

Fix or reclassify all 29 findings in the [area D lifetime audit](cppgw-lifetime-d-20261118.md) against the
integrated source at `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The audit-only merge is
`dc3abc2f904b8d93a8053840c05b772938841beb`. The first source unit addresses the six private race losers, followed by
startup shutdown ordering, the OpenRouter index, callback ownership, cycles and await expressions.

| Surface                                                           | Required behavior                                                                                                                              |
| ----------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Body reads, Responses, telemetry, provisioning, replay and health | Preserve the deciding result, deadlines and late operation observation. Dispose of synthetic losers once the race settles.                     |
| Startup and catalog                                               | Server startup can overlap migration. Finish the migration before KV closure. Publish each catalog snapshot with its own index.                |
| Admin, auth and terminal hooks                                    | Preserve callback inputs, failure types, rejection timing, settlement, request drain and accounting. A pending callback must own its callable. |
| Await expressions and release                                     | Follow the GCC 12.2 comparison rules. Test the supported Linux ARM64 build and publish the scoped change for review.                           |

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
column identifies the source commit. The gate table records its full SHA and tree.

## Defect ledger

| Audit ID                                          | Verdict                                                                                                                                                                                                                  | Commit                                                   | Test                                                                                                                                               |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/bounded_response_body.cpp:41`                | FIXED · `abortableRead` settles its private abort latch after unregistering the listener, releasing each completed read result.                                                                                          | Settle private race losers (`cc566ea1`)                  | `bounded_response_body_test` measures retained 64 KiB chunks on a live loop.                                                                       |
| `src/responses_stream.cpp:287`                    | FIXED · `readWithDeadline` settles its private inactivity latch after unregistering the timeout listener.                                                                                                                | Settle private race losers (`cc566ea1`)                  | `responses_stream_test` reads text, a 64 KiB comment and a terminal event before sampling allocations.                                             |
| `include/uos/optional_telemetry_queue.hpp:258`    | FIXED · `waitForProgress` finishes the private timeout promise as it clears the timer.                                                                                                                                   | Settle private race losers (`cc566ea1`)                  | `optional_telemetry_queue_differential_test` repeats successful writer waits and checks the existing timing traces.                                |
| `src/auth/local_development_key.cpp:97`           | FIXED · `awaitWithAbort` settles the observed private abort promise after detaching it. The pricing work stays observed if the abort wins.                                                                               | Settle private race losers (`cc566ea1`)                  | `local_development_key_test` provisions fresh in-memory stores and measures retained allocations.                                                  |
| `src/sentinel/replay_retention_bootstrap.cpp:469` | FIXED · `settledWithin` resolves the private wait after clearing its timer and before delivering the original result.                                                                                                    | Settle private race losers (`cc566ea1`)                  | `sentinel_replay_retention_test` repeats completed owner fills against fresh in-memory stores.                                                     |
| `src/health.cpp:306`                              | FIXED · `probeUpstreamCoalesced` settles the private timeout promise when clearing the timer and the coalescing slot.                                                                                                    | Settle private race losers (`cc566ea1`)                  | `health_test` checks one configured mock probe and measures the health timer across repeated unconfigured probes.                                  |
| `src/serve.cpp:123`                               | FIXED · Both launchers retain the migration. Normal shutdown and prepared startup failure await it before telemetry and KV close.                                                                                        | Drain startup migration before KV closure (`d5174428`)   | `serve_test` delays the migration read and forces the loopback listener setup callback to fail, then checks close ordering and the original error. |
| `src/models/openrouter_models.cpp:158`            | FIXED · `buildMatchIndex` takes raw row pointers. Complete the index before replacing the cache or index, so the failed-refresh fallback keeps both old owners.                                                          | Publish catalog and index together (`4e525a2d`)          | `openrouter_models_test` injects an allocation failure at the index build and checks the prior owner's lifetime, lookups and a successful retry.   |
| `src/admin/codex.cpp:207`                         | FIXED · The whitelist read can reject while the catalog override is suspended. The child awaits with the invoked callable owned.                                                                                         | Retain invoked callbacks through settlement (`3eafbfe1`) | `admin_codex_coverage_test` rejects the whitelist read before resuming a captured callback.                                                        |
| `src/admin/codex.cpp:216`                         | FIXED · Any refresh child can reject while an override stays suspended. The five override tasks retain their callable and receive value options.                                                                         | Retain invoked callbacks through settlement (`3eafbfe1`) | `admin_codex_coverage_test` rejects Surplus before releasing a pending OpenLux override.                                                           |
| `src/admin/codex.cpp:238`                         | FIXED · Provider selection uses the uncaught strong-read error path, so it can reject the join. The catalog child now retains its callable.                                                                              | Retain invoked callbacks through settlement (`3eafbfe1`) | `admin_codex_coverage_test` rejects the selection read and resumes the pending catalog override.                                                   |
| `src/admin/defaults.cpp:53`                       | FIXED · A rejected Kernel limit read can leave the diagnostics override pending. Its Task now holds the callable.                                                                                                        | Retain invoked callbacks through settlement (`3eafbfe1`) | `admin_defaults_differential_test` rejects the limit read first and resumes diagnostics afterwards.                                                |
| `src/handler/terminal_log.cpp:61`                 | FIXED · The first writer can suspend before a later factory throws. Each injected writer's original promise now holds the callable until settlement.                                                                     | Retain invoked callbacks through settlement (`3eafbfe1`) | `prompt_cache_telemetry_gate_test` throws from the second writer and checks that the first resumes after the response.                             |
| `src/handler/terminal_route.cpp:523`              | FIXED · Background registration permits the caller to leave while the injected persistence Task is still suspended. The existing completion chain keeps its callable.                                                    | Retain invoked callbacks through settlement (`3eafbfe1`) | `handler_replay_background_test` resumes the callback after its scheduling coroutine has returned.                                                 |
| `src/handler/terminal_log.cpp:519`                | LEFT · `sendResponse` prepares native headers before taking the body. A header rejection can deliver a fallback 500 without settling the original SSE body.                                                              | Transport owner                                          | The cancellation belongs to `src/runtime/http.cpp`, which is outside this branch's assigned paths.                                                 |
| `src/inference_admission.cpp:170`                 | FIXED · Pending waiters used to own the abort listener that retained both waiter and controller. The exact unreferenced timer now owns the controller externally, and teardown can release the weak listener and waiter. | Break listener and server ownership cycles (`55424060`)  | `oss_inference_admission_test` abandons a queued wait with no other owners and observes the caller signal expire at loop teardown.                 |
| `src/kv.cpp:56`                                   | FIXED · `setKvForTest` can replace the selected opener while its Task is suspended. The awaiting open frame now owns the exact invoked copy.                                                                             | Retain invoked callbacks through settlement (`3eafbfe1`) | `storage_bun_kv_test` replaces the opener, resumes the earlier attempt and checks the returned store.                                              |
| `src/jev_compaction/compaction.cpp:238`           | FIXED · The wrapper could be destroyed with a buffered chunk and no further pulls. Its state destructor now clears the timer and removes the forwarding listener from the live external signal.                          | Break listener and server ownership cycles (`55424060`)  | `jev_compaction_test` discards buffered responses repeatedly with the same signal and samples live allocations.                                    |
| `src/lib/jev_compaction/jev_codex_smoke.cpp:180`  | FIXED · The fake owns its server handle, which previously retained the fake through the registered handler. Registration is weak and each request holds a locked owner.                                                  | Break listener and server ownership cycles (`55424060`)  | `jev_codex_smoke_test` starts and stops the loopback fake and checks that its owner expires.                                                       |
| `src/serve.cpp:159`                               | FIXED · `process::once` stores its callback before arming the signal wait. The development launcher's cleanup guard now covers both registrations.                                                                       | Drain startup migration before KV closure (`d5174428`)   | `serve_test` covers startup cleanup. Faulting a valid OS signal registration requires intrusive failure injection.                                 |
| `src/handler/serve_handler.cpp:13`                | FIXED · The request signal owned a listener that held the same signal. The handoff owns the signal and the listener now holds it weakly.                                                                                 | Break listener and server ownership cycles (`55424060`)  | `serve_delivery_http_test` drops a handoff without calling it and checks that the request signal is released.                                      |
| `src/admin/defaults.cpp:99`                       | FIXED · `applyDefaultsUpdateAttempt` now binds the atomic result before reading `ok`.                                                                                                                                    | Name owning coroutine inputs and results (`b9d90942`)    | Existing `admin_defaults_differential_test` commit paths.                                                                                          |
| `src/auth/passkeys.cpp:4313`                      | FIXED · All six audited lookups or deletions still constructed their keys in the await expression. Each now receives a named key.                                                                                        | Name owning coroutine inputs and results (`b9d90942`)    | Existing `passkeys_test`, `passkeys_part1_test` and `passkeys_part2_test`.                                                                         |
| `src/admin/codex.cpp:71`                          | FIXED · The debug-routing DELETE handler names the reset scenario before awaiting.                                                                                                                                       | Name owning coroutine inputs and results (`b9d90942`)    | Existing DELETE fixture in `admin_codex_coverage_test`.                                                                                            |
| `src/admin/kernel.cpp:380`                        | FIXED · Cohort hashing, reset and overage reads, and the overage write now use named strings.                                                                                                                            | Name owning coroutine inputs and results (`b9d90942`)    | Existing `admin_kernel_coverage_test`.                                                                                                             |
| `src/auth/index.cpp:137`                          | FIXED · The GitHub cache input and policy queue path are built before awaiting. The digest call takes ownership of the assembled input.                                                                                  | Name owning coroutine inputs and results (`b9d90942`)    | Existing `auth_responses_branch_coverage_test`.                                                                                                    |
| `src/models/codex_models_whitelist.cpp:134`       | FIXED · The strong-read options and Task are named before awaiting.                                                                                                                                                      | Name owning coroutine inputs and results (`b9d90942`)    | Existing whitelist and failure fixtures in `admin_codex_coverage_test`.                                                                            |
| `src/responses_failover_stream.cpp:944`           | FIXED · Both `emitFailureTerminal` calls move their event inputs into a named Task before awaiting it.                                                                                                                   | Name owning coroutine inputs and results (`b9d90942`)    | Existing `responses_failover_stream_test` and `responses_failover_branch_coverage_test`.                                                           |
| `include/uos/responses_stream.hpp:125`            | FIXED · `MaybeAsyncCallback::run` starts a named Task with its owned event or failure aggregate.                                                                                                                         | Name owning coroutine inputs and results (`b9d90942`)    | Existing `responses_stream_test` and `responses_failover_stream_test`.                                                                             |

## Verification and handoff

The audit records zero UNCLEAR rows. In the integrated `rt::race` implementation, each pending source still owns
a reaction that holds the combined result. Cleanup at all six sites removed the callback able to settle a private
source. The new tests sample ASan's live allocation count before the loop is destroyed. The loop's teardown
otherwise removes pending reactions and can leave LeakSanitizer clean even after a long-running process retains
memory. The checked behavior includes the first event, inactivity, timeout and abort results and the existing
microtask traces.

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
bash "$scratch/run-gate.sh" asan 2 python3 -B "$scratch/select.py" cpp/gateway changes \
  "${all_changed_paths[@]}" -- --config asan --shard "$shard/4"
bash "$scratch/run-gate.sh" release 2 bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
python3 cpp/gateway/tools/links.py --check
git diff --check
```

Strict `clang++-21 -fsyntax-only` checks ran with the driver's warnings, C++20, sanitizer, include and native
dependency flags before the source commits. `python3 cpp/gateway/tools/links.py --check` passed all 450 entries on
the source candidate. `git diff --check aae39d0e..HEAD` passed.

The six source commits end at `b9d90942d56bda718599cdd39a4093d7206e499c`, tree
`c409743e10cafe2c4cfb917060a5280e2b5a94e3`. There are 18 new cases in 17 existing test binaries. The final
sanitizer selection contains 277 of the 450 registered binaries. The restart checkpoint for local toolchain,
build caches and verification receipts is `/var/tmp/cppgw-lifetime-fix-d.8WiFYc9c`. A build's
terminal receipt records the tested commit, source digest, result counts and the full compiler configuration.

### Per-commit gates

The binary and case totals apply separately to release and ASan. Wall times are seconds, including each driver's
compilation and isolated test execution. The copies under `receipts/` use the unit name and configuration.

| Unit         | Commit                                     | Tree                                       | Binaries per profile | Cases per profile | Release wall (s) | ASan wall (s) |
| ------------ | ------------------------------------------ | ------------------------------------------ | -------------------: | ----------------: | ---------------: | ------------: |
| Races        | `cc566ea14ba4dde56a105f91847f8b95f6d6ae8f` | `8aa11d9379b32eaea28cd292c5b1deb3382eb304` |              218/218 |       3,132/3,132 |           78.257 |       225.582 |
| Startup      | `d51744286f8056b448905a2901ec51ea52d6de0b` | `473015577ea6946c157088ef1dfc07f55cf89dea` |                  3/3 |             19/19 |           13.039 |        15.307 |
| Catalog      | `4e525a2d8b37e72503294b4ff6fb207ca457a0c8` | `9091b8d1db4023580cc0686e2ba4b8f197d6384f` |              129/129 |       1,809/1,809 |          536.445 |       783.443 |
| Callbacks    | `3eafbfe1d02698552d9f95d414f5ee08fd8d9d88` | `babc9578abd100a0b413a21f5239e3b45377bf56` |              231/231 |       3,086/3,086 |          273.445 |       497.255 |
| Cycles       | `554240606eabd67d42e4dc8c98de37e43e3cf355` | `094bbd58dc9de3351e4d2743dd0b32cabceeb89e` |                51/51 |           623/623 |           75.023 |       129.316 |
| Named awaits | `b9d90942d56bda718599cdd39a4093d7206e499c` | `c409743e10cafe2c4cfb917060a5280e2b5a94e3` |              222/222 |       3,019/3,019 |        1,546.652 |     2,163.058 |

### Final source gates

All five receipts record commit `b9d90942d56bda718599cdd39a4093d7206e499c`, tree
`c409743e10cafe2c4cfb917060a5280e2b5a94e3` and unchanged native inputs SHA-256
`7abf14bf3b1d3202a4b3213e6b991014ae08cae1be4de4c32128c5a503da350b`. The sanitizer shards partition the
sorted list of all 277 affected test binaries. The release gate is the unfiltered first shard. The release shard
reused the separately warmed objects, which took 141.307 s to prepare.

| Gate                     | Binaries passed | Cases passed | Driver wall (s) | Receipt SHA-256                                                    |
| ------------------------ | --------------: | -----------: | --------------: | ------------------------------------------------------------------ |
| Release 1/4              |         113/113 |  1,339/1,339 |          48.433 | `f1e9701c72b4e89f3ea7091839fa4fd42c4dd26245365e1e1253d4d53085a9fa` |
| ASan, UBSan and LSan 1/4 |           70/70 |      906/906 |          39.729 | `4952aa2f2eace4aa110049d4265b19415616df0be038740b00f62bd0e38b9d6f` |
| ASan, UBSan and LSan 2/4 |           69/69 |      999/999 |          27.556 | `10fd72389c50b8cf821a73fec0cae0585c743bf247c1ed1b56e39935b7690163` |
| ASan, UBSan and LSan 3/4 |           69/69 |      731/731 |          36.790 | `6df2b6a388aab3d922aaa0c4a180e4e181290aca0709c60739e0554f72e15958` |
| ASan, UBSan and LSan 4/4 |           69/69 |  1,017/1,017 |         126.873 | `e118e0a453d426c91de6c7ea237c5e2babaceb300945e3e4246b1b96e9c28669` |
| Sanitizer total          |         277/277 |  3,653/3,653 |                 |                                                                    |

### Before-fix controls

The isolated ASan driver ran the 17 regression binaries using the integrated implementation from
`fca4baf20114b7a4bd602eff2a14fcaf4598f11a` and the committed regression fixtures. The OpenRouter control retained
the test fault and restored the old publication order. All 17 binaries compiled, linked and failed as expected.
The harness reported 249 cases, 234 passing and 15 failing. Three binaries aborted on the LeakSanitizer findings
below before flushing a case summary. The overlay ran in 397.767 s, including 378.714 s of compilation and 18.590 s
of linking and tests. Its input SHA-256 is `0d4ddbb44d78385f2dd042a03ef84057fe970812347b0defd2ebb1864ad66d19`.
The SHA-256 of `receipts/baseline-witnesses.json` is
`9f4f5f4069d7de4bfe9203a8be0fe23e949ba15ed01e5ad69b8f9396b9a9586d`. The verification checkout returned to the
clean source commit after the controls.

```bash
bash "$scratch/run-gate.sh" asan 4 python3 -B "$scratch/select.py" cpp/gateway only \
  "${regression_test_paths[@]}" -- --config asan
```

| Before-fix fixture                      |           Observed retention or leak |
| --------------------------------------- | -----------------------------------: |
| Bounded body reads, 32 samples          |                 2,120,192 live bytes |
| Responses stream reads, 32 samples      |                 2,135,904 live bytes |
| Telemetry writer waits, 128 samples     |                    35,840 live bytes |
| Local provisioning, 64 samples          |                   142,272 live bytes |
| Replay owner fills, 128 samples         |                    33,792 live bytes |
| Health probes, 64 samples               |                    89,856 live bytes |
| Discarded compaction bodies, 64 samples |                    16,256 live bytes |
| Abandoned queued admission              | 1,504 leaked bytes in 17 allocations |
| Dropped delivery handoff                |    504 leaked bytes in 6 allocations |
| Stopped smoke server                    | 2,599 leaked bytes in 22 allocations |

The other before-fix failures came from the migration close ordering assertion, the catalog ownership assertion
and the suspended-callback assertions for admin, terminal logging, background replay and the KV opener. All 17
regression binaries passed with the fixes in the final sanitizer partition.

### Health fixture isolation

The first race snapshot `4d2d475ffde8794baf5a0180cc699902e5b0f4b1` passed 218 release binaries and 3,132 cases.
Its ASan gate passed 217 binaries and 3,131 cases. The failing new health case measured 277,888 live bytes across
64 configured probes. That setup also traverses the separate Codex auth race described below. An unconfigured
variant with counting KV and per-sample JSON decoding measured 96,768 live bytes and also failed. The committed
fixture asserts one configured mock response, checks the unconfigured payload once, then samples the gateway's
completed health race with plain in-memory KV. The focused sanitizer rerun passed all 23 health cases in 18.442 s.
Both first-run receipts and the focused reruns are retained in the scratch directory. The six source units were
replayed with the corrected fixture in the first unit before the committed-tree gates.

### Startup fixture isolation

The invalid-port variant passed all 19 release cases in three binaries. LeakSanitizer then reported 2,392 bytes in
18 allocations from `runtime::http::serveHttp`, starting at `ServeState` and its native server registrations.
`Server::Impl::emitErrorLater` rejects startup without emitting close or releasing the server's self-owning
registrations. This is a separate transport-owned finding. The committed fixture binds an ephemeral loopback port
and injects an error into the existing `onListen` callback via the console observer. That startup fault exercises the
same launcher failure cleanup and reaches the native server's close path. The focused ASan rerun passed all six
serve cases in 9.843 s. The startup unit and subsequent units were replayed with that fixture.

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

## Delivery

The branch preserves the integration baseline and the audit merge. Its report-only commits leave the native input
digest identical to the five final receipts. `origin/development` refreshed to
`5ddac3dd154d74b1049197fb7cf5f53551609072` and does not contain the task tip. The checkout remains on the clean,
committed task branch for the pull request handoff.

```bash
git fetch origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 timeout 20 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-lifetime-fix-d-20261118
GIT_TERMINAL_PROMPT=0 timeout 20 git push origin HEAD:refs/heads/agent-jobs/cppgw-lifetime-fix-d-20261118
```

The dry run and push both exited 128 with `fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
GitHub publication and its hosted CI receipt are pending access to a write credential.

## Summary

Fixed 28 of 29 lifetime findings in the native gateway. Completed requests release buffers promptly. Startup
migration finishes before KV closes, catalog refresh keeps its data and lookups in sync, and suspended callbacks
retain the state they use.

The remaining audit finding belongs to HTTP transport. A header setup error can leave an unread SSE response waiting
after the fallback response finishes. The report also records separate transport startup and Codex auth retention issues.

All six fix units passed release and sanitizers. The final Linux ARM64 checks passed 277 sanitizer binaries and
113 release binaries. All 17 before-fix controls caught their regressions, and the link registry passed.

The work is committed locally. The runner has no usable GitHub credentials for the push, which leaves the pull
request and hosted CI pending.
