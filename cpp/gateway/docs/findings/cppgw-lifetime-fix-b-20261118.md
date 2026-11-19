# Native gateway server and storage lifetime fixes

## Work contract v1

Recheck D01–D14 against integration base `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`
with audit tip `fa1cd486b94214e5110529a35fd7c4e0ff5c93a9` merged. First fix HTTP
write ownership, then WebSocket shutdown, then ownership and teardown, then the
comparison-compiler expressions. Keep each fix with its evidence in one commit.

| Surface | Required behavior |
| --- | --- |
| HTTP and WebSocket | Keep wire bytes, status and close codes, backpressure, and actual delivery receipts. Shutdown still aborts and drains its admitted work in order. |
| Worker boundary | Exercise refused, cancelled, dropped, and delivered bodies using local mocks. Preserve one settlement using the semantic terminal and delivery outcome. |
| Kernel, cache, and KV | Preserve quota timing, key and value bytes, consistency, and error types. Exercise lease teardown with fake time. |

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

| ID | Verdict | Commit | Test |
| --- | --- | --- | --- |
| D01 | FIXED — Closing clears queued writes and keeps the active buffer through Asio's completion callback. | `fix(cpp): retain HTTP write buffer through cancellation` | `runtime_http_lifetime_test`: ASan baseline failed the buffer-liveness assertion, then 5/5 passed under ASan and release. |
| D02 | FIXED — Failed startup now closes the low-level server after rejecting both public receipts, releasing its handler registrations. | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_lifetime_test`: the failed-bind handler survived on the baseline and expires after the fix. |
| D03 | FIXED — A waiting frame owns the drain listeners. Event dispatch acquires a weak target and frame destruction removes the registrations. | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_test`: backpressure, drain, and disconnect. The public HTTP path closes or aborts before it can abandon a drain wait in isolation. |
| D04 | FIXED — The request frame unregisters the early SSE entry on destruction, including while its handler is still pending. | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_lifetime_test`: a disconnected request survived loop destruction on the baseline and expires after the fix. |
| D05 | FIXED — The abort callback weakly observes the signal, and the sender removes the listener on frame destruction. | `fix(cpp): release HTTP listeners on startup failure and teardown` | `runtime_http_test` and `runtime_http_sse_headers_test`: pending reads, interruption, and streaming. Abandonment without abort is only exposed during forced loop destruction; a socket-only fixture also introduces connection teardown lifetimes. |
| D06 | FIXED — Recheck shutdown and disconnect after the module await, before the synchronous WebSocket handoff. | `fix(cpp): refuse WebSocket upgrades that race shutdown` | `runtime_http_lifetime_test`: ASan baseline accepted the upgrade, then 6/6 passed in ASan and release with a 503 refusal. |
| D07 | LEFT — Memory KV await-expression reverification pending. | — | Pending |
| D08 | LEFT — Cache fingerprint await-expression reverification pending. | — | Pending |
| D09 | FIXED — The last public terminal callback stops periodic renewal when there is no terminal intent. The last stored lease can expire naturally. | `fix(cpp): stop renewing abandoned kernel reservations` | `kernel_quota_context_differential_test`: a retained callback copy renews the lease. Dropping it leaves the expiry unchanged and later aborts with `TimeoutError`. |
| D10 | LEFT — Worker body read listener reverification pending. | — | Pending |
| D11 | LEFT — Worker request bridge reverification pending. | — | Pending |
| D12 | LEFT — Worker delivery abort listener reverification pending. | — | Pending |
| D13 | LEFT — Pending delivery receipt retention reverification pending. | — | Pending |
| D14 | LEFT — Quota and usage await-expression reverification pending. | — | Pending |

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

The baseline passed 5/6 cases in 31.641 s. The fixed ASan differential fixtures
passed 2 binaries and 22/22 cases in 31.403 s. A broader `kernel_quota` selection
was interrupted during its cold compilation to finish the focused regression
first. Its compiled objects remain in the scratch cache.

[Area B audit](cppgw-lifetime-b-20261118.md)
