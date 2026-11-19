# Native gateway server and storage lifetime fixes

## Work contract v1

Recheck D01–D14 against integration base `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`
with audit tip `fa1cd486b94214e5110529a35fd7c4e0ff5c93a9` merged. First fix HTTP
write ownership, then WebSocket shutdown, then ownership and teardown, then the
comparison-compiler expressions. Keep each fix with its evidence in one commit.

| Surface               | Required behavior                                                                                                                                       |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| HTTP and WebSocket    | Keep wire bytes, status and close codes, backpressure, and actual delivery receipts. Shutdown still aborts and drains its admitted work in order.       |
| Worker boundary       | Exercise refused, cancelled, dropped, and delivered bodies using local mocks. Preserve one settlement using the semantic terminal and delivery outcome. |
| Kernel, cache, and KV | Preserve quota timing, key and value bytes, consistency, and error types. Exercise lease teardown with fake time.                                       |

The edit scope is the assigned C++ runtime, RT storage and platform files, storage,
cache, kernel and worker modules and headers, their tests and registry entries, and
this report. Provider implementations, TypeScript, workflows, C++ outside the
gateway, deployment, and live storage are outside this work. Fixture runs use a
fresh home and temporary directory in the driver's isolated loopback namespace
with a credential-free environment.

Use `bash cpp/gateway/tools/ci-build.sh --config release <test>` and the matching
`--config asan` for link-dependent tests after each family. Use all four sanitizer
shards for the final affected set, one full release shard, and
`python3 cpp/gateway/tools/links.py --check`. Reverify stale audit line numbers and
interleavings before editing. The unknowns are the current reachability of each
finding and whether the fixture I/O backend can observe D01's buffer lifetime
under ASan. A cold driver configuration compiles the production object graph once.
Check progress through incremental driver logs and reuse the object cache for
focused verification. Stop at verified verdicts for all rows or commit the
building subset with the pending rows and their evidence gaps stated.

## Defect ledger

A commit listed as “this commit” is the commit that introduced that verdict or fix.

| ID  | Verdict                                                                                                                                                                        | Commit                                                             | Test                                                                                                                                                                                                                                                |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D01 | FIXED — Closing clears queued writes and keeps the active buffer through Asio's completion callback.                                                                           | `fix(cpp): retain HTTP write buffer through cancellation`          | `runtime_http_lifetime_test`: ASan baseline failed the buffer-liveness assertion, then 5/5 passed under ASan and release.                                                                                                                           |
| D02 | FIXED — Failed startup now closes the low-level server after rejecting both public receipts, releasing its handler registrations.                                              | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_lifetime_test`: the failed-bind handler survived on the baseline and expires after the fix.                                                                                                                                           |
| D03 | FIXED — A waiting frame owns the drain listeners. Event dispatch acquires a weak target and frame destruction removes the registrations.                                       | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_test`: backpressure, drain, and disconnect. The public HTTP path closes or aborts before it can abandon a drain wait in isolation.                                                                                                    |
| D04 | FIXED — The request frame unregisters the early SSE entry on destruction, including while its handler is still pending.                                                        | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_lifetime_test`: a disconnected request survived loop destruction on the baseline and expires after the fix.                                                                                                                           |
| D05 | FIXED — The abort callback weakly observes the signal, and the sender removes the listener on frame destruction.                                                               | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_test` and `runtime_http_sse_headers_test`: pending reads, interruption, and streaming. Abandonment without abort is only exposed during forced loop destruction; a socket-only fixture also introduces connection teardown lifetimes. |
| D06 | FIXED — Recheck shutdown and disconnect after the module await, before the synchronous WebSocket handoff.                                                                      | `fix(cpp): refuse WebSocket upgrades that race shutdown`           | `runtime_http_lifetime_test`: ASan baseline accepted the upgrade, then 6/6 passed in ASan and release with a 503 refusal.                                                                                                                           |
| D07 | FIXED — Memory KV delete names the commit result before reading `ok`. The audited expression violated the GCC 12 comparison rule.                                              | `fix(cpp): name awaited KV keys and fingerprint inputs`            | `rt/kv_test`: existing delete coverage. The comparison-rule fix has no observed modern sanitizer failure.                                                                                                                                           |
| D08 | FIXED — Each of the four fingerprint hashes receives a named canonical string in a separately constructed task.                                                                | `fix(cpp): name awaited KV keys and fingerprint inputs`            | `prompt_cache_scope_targets_test`: existing fingerprint behavior. The finding is a comparison-rule violation.                                                                                                                                       |
| D09 | FIXED — The last public terminal callback stops periodic renewal when there is no terminal intent. The last stored lease can expire naturally.                                 | `fix(cpp): stop renewing abandoned kernel reservations`            | `kernel_quota_context_differential_test`: a retained callback copy renews the lease. Dropping it leaves the expiry unchanged and later aborts with `TimeoutError`.                                                                                  |
| D10 | FIXED — A weak signal capture and frame-owned registration release the body reader on abandonment.                                                                             | `fix(cpp): release worker request and response state`              | `worker_boundary_test`: an abandoned read releases its stream even when the caller keeps the signal.                                                                                                                                                |
| D11 | FIXED — An owned abort registration forwards through a weak signal, transfers into delivery, and unregisters on teardown.                                                      | `fix(cpp): release worker request and response state`              | `worker_boundary_test`: loop teardown during authentication releases the request.                                                                                                                                                                   |
| D12 | FIXED — The body owns its delivery state. Cancellation observes that state weakly and releases its registration on terminal paths and destruction.                             | `fix(cpp): release worker request and response state`              | `worker_boundary_test`: an unread body and upstream response expire after drop, including when abort preceded delivery setup.                                                                                                                       |
| D13 | FIXED — A host receipt retains the shared settlement latch and abort registration. The dropped body, reader, and host lifetime callable can expire before the receipt arrives. | `fix(cpp): release worker request and response state`              | `worker_boundary_test`: a pending receipt outlives the released response body, then settles one admission as delivered.                                                                                                                             |
| D14 | FIXED — Separately construct the two quota and six usage KV keys and read tasks before awaiting them.                                                                          | `fix(cpp): name awaited KV keys and fingerprint inputs`            | `kernel_quota_values_differential_test` and `kernel_usage_coverage_test`: existing projection and readback coverage. The finding is a comparison-rule violation.                                                                                    |

## Verification and open questions

The audit merged cleanly and added four files under its findings and analysis
paths. The ARM64 runner uses Clang 21.1.8, Boost 1.92.0 static libraries, ICU 78.3,
and SQLite 3.54.0 with double-quoted-string support.

`ConnectionState::flush` handed Asio a pointer into `output.front()`.
`ConnectionState::destroy` could clear the deque before Asio dispatched the
cancelled completion. The regression sends an 8 MiB response to a loopback peer
that does not read, verifies pending output, and closes the socket in the write
turn. The ASan allocator hook observes the connection's payload copy so the test
can check its liveness even when the OS cancels without dereferencing the buffer.

Focused commands used the CI driver's namespace and test runner with the test's
registered link sources and all 44 RT translation units. A scratch selector
limited compilation to those sources for the pre/post-fix regression.

```sh
scratch=/tmp/cppgw-lifetime-fix-b.03wOF6
bash "$scratch/run.sh" 4 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config release
```

The corrected baseline fixture reported 4/5 cases in 9.709 s. Its sole failure
was the ASan buffer-liveness assertion. The fixed tree reported 5/5 cases in
27.917 s with ASan/UBSan/LSan and 5/5 cases in 125.103 s on a cold focused release
build. Full linked-test verification remains open.

The D06 fixture queues shutdown from the authorization callback. The server's
ready wait resumes after authorization and before the cached module wait resumes.
The old code accepted the socket after the shutdown snapshot. The fixed code
preserves the immediate refusal when shutdown was already observed, and returns
the same 503 when shutdown begins during module loading.

```sh
bash "$scratch/run.sh" 4 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" src/runtime/http_upgrade.cpp --config asan runtime_http
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config release runtime_http
```

The baseline fixture passed 5/6 cases in 12.065 s. The fixed ASan selection
passed 5 binaries and 37/37 cases in 93.907 s. The release fixture passed 6/6
cases in 7.946 s.

For D02–D05, `serveHttp` never reached the listener cleanup on listen error.
`waitForDrain` stored four callbacks capturing their own state, the early SSE
registry callback owned its key, and the send abort listener owned its own
signal. The failed-bind and abandoned-handler regressions both failed their
liveness assertions with the old code. LSan reported 6,205 bytes in 53
allocations in that process. The first cleanup attempt exposed an additional
`ServeState` cycle in the unused or unremoved shutdown listener. The listener
now weakly acquires the state and unregisters when that state expires.

```sh
bash "$scratch/run.sh" 4 python3 -B "$scratch/focused.py" tests/runtime_http_lifetime_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" src/runtime/http.cpp,src/runtime/http_early_sse.cpp --config asan runtime_http
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" src/runtime/http.cpp,src/runtime/http_early_sse.cpp --config release runtime_http
```

The corrected baseline exited with LSan diagnostics in 14.646 s and did not
flush a case summary. The fixed selections passed 6 binaries and 42/42 cases in
40.573 s on release and 56.102 s under ASan/UBSan/LSan.

The D09 context's interval owned a shared lease context after both handles were
dropped. The regression kept one copied callback through a renewal, then deleted
it. The baseline extended the expiry from `1800000360000` to `1800000420000`
and kept the signal live. The fixed context preserves already-started renewal
and terminal-intent retry work. Clearing its interval prevents subsequent
renewals while the existing expiry timer and durable reclaim path remain active.

```sh
bash "$scratch/run.sh" 4 python3 -B "$scratch/focused.py" tests/kernel_quota_context_differential_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" tests/kernel_quota_context_differential_test.cpp,tests/kernel_quota_reserve_differential_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" tests/kernel_quota_context_differential_test.cpp,tests/kernel_quota_reserve_differential_test.cpp --config release
```

The baseline passed 5/6 cases in 31.641 s. The fixed differential fixtures
passed 2 binaries and 22/22 cases in 31.403 s under ASan and 45.554 s on
release. A broader `kernel_quota` selection was interrupted during its cold
compilation to finish the focused regression first. Its compiled objects
remain in the scratch cache.

`readBoundedJson` and `boundaryFetch` registered callbacks with strong paths
back to their own signal. `deliverResponse` stored a callback owning the same
delivery object, and the host receipt held a reaction that owned the entire
response. Four regression cases reproduced these retentions. The original
sanitizer run reported 31,947 leaked bytes in 243 allocations and did not flush
a case summary. A fixture iteration also identified the stream start reaction's
temporary ownership. The final fixture lets that reaction finish before checking
the still-pending receipt. The late receipt supplies the real outcome to the
same settlement as body cancellation or EOF, gated by one completion promise.

```sh
bash "$scratch/run.sh" 4 python3 -B "$scratch/focused.py" tests/worker_boundary_test.cpp --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" src/worker/entrypoint.cpp --config asan worker_boundary_test
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" src/worker/entrypoint.cpp --config release worker_boundary_test
```

The baseline took 34.134 s. The fixed fixture passed 10/10 cases in 24.535 s
under ASan and 13.081 s on release.

For D07, D08, and D14, the current sites matched the audit. The fixes follow
the retained GCC 12 coroutine rules by naming results, inputs, and tasks
before projection or suspension. The Clang 21 sanitizer gate exercises the
existing storage, fingerprint, and quota behavior. The GCC 12 comparison SDK
is outside this runner's driver profiles, so there is no GCC 12 crash receipt.

```sh
selection=tests/rt/kv_test.cpp,tests/prompt_cache_scope_targets_test.cpp,tests/kernel_quota_values_differential_test.cpp,tests/kernel_usage_coverage_test.cpp
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" "$selection" --config asan
bash "$scratch/run.sh" 2 python3 -B "$scratch/focused.py" "$selection" --config release
```

The focused selections passed 4 binaries and 88/88 cases in 357.486 s on
release and 616.335 s under ASan/UBSan/LSan.

## Unclear and adjacent findings

| Audit site                                      | Current finding                                                                                                                                                                                                                                                                                                                                           |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Standalone `node_http` request timeout          | The shipped HTTP entry uses `serveHttp`, which sets `requestTimeout` to zero. The lower-level timeout/refusal path remains a library concern for a caller using that option with response-specific delivery handling.                                                                                                                                     |
| Active listener with forced loop destruction    | The gateway entrypoint runs on the process-lifetime loop and shuts the HTTP listener down explicitly. The D04 fixture covers teardown after listener closure and found the additional `ServeState` abort cycle fixed with the teardown changes. Forced destruction of a still-listening reusable loop remains outside that proof.                         |
| SQLite completion after forced loop destruction | The launcher waits for the server and durable request work, drains telemetry, and closes the KV worker. `runJob` still borrows the Asio context. Destruction of an independently scoped loop with a queued SQLite job remains outside the launcher contract.                                                                                              |
| Abandoned quota lock turn                       | Each current holder directly awaits its operation and hands the lock to the next turn on success or exception. Request cancellation travels through signals while the lock operation continues. A forced reusable-loop teardown can leave a static lock entry pending, and clearing queued turns safely would require preserving their predecessor order. |

An independent retention defect in `src/rt/fetch.cpp` is outside this change's
ownership. `makeBodyAbortable` gives the wrapper stream an algorithm that owns
`AbortableBody`. The body stores a strong controller back to the wrapper. An
unread response dropped without EOF, abort, or cancellation can keep that cycle
alive. A scratch-only local mock reproduced the retained source marker and LSan
reported 4,120 bytes in 32 allocations. The probe compiled in 7.149 s and
exited through SIGTRAP in 1.407 s inside the fixture namespace.

```sh
bash "$scratch/run.sh" 1 python3 -B cpp/gateway/analysis/lifetime/b/probe.py \
  "$scratch/fetch-probe-base" "$scratch/fetch_body_probe.cpp" tests/rt/fetch_test.cpp
```

## Final verification and handoff

`http.cpp` and `http_upgrade.cpp` use the HTTP lifetime, differential, streaming,
and WebSocket fixtures. `entrypoint.cpp` uses the worker-boundary fixture.
`quota_reservations.cpp`, `quota_v2.cpp`, `usage.cpp`, and `scope_targets.cpp`
use the quota, usage, and scope-target fixtures. `kv_memory.cpp` is a shared RT
source linked into all 450 tests. The final ASan selection therefore uses the
four full shards and the release gate uses full shard 1.

```sh
bash "$scratch/run.sh" 2 bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
for shard in 1 2 3 4; do
  bash "$scratch/run.sh" "$jobs" bash cpp/gateway/tools/ci-build.sh --config asan --shard "$shard/4"
done
python3 cpp/gateway/tools/links.py --check
git diff --check aae39d0ea73e7b5678dc92c437f7da6fb32f0c06 HEAD
```

Run with two workers per configuration while both are active, then use the
remaining four workers for subsequent shards. The driver writes per-object
logs, incremental case totals, and a receipt with Git and input hashes. The
partially warm first build is expected to take tens of minutes on four cores.
Subsequent shards reuse production objects. A failing gate preserves its log for
diagnosis and leaves the branch unpublished. Final receipts are pending.

The D06 follow-up (`fix(cpp): name the loaded WebSocket module before storing it`)
names the value returned by the module await before moving it into the optional.
The lifetime fixture passed 8/8 cases in 11.112 s under ASan and 7.327 s on
release. The earlier full release build was interrupted to bind final receipts
to the corrected tree.

[Area B audit](cppgw-lifetime-b-20261118.md)
