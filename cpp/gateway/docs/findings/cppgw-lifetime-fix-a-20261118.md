# Native gateway runtime lifetime fixes, area A

## Work contract v1

Close the confirmed coroutine, callback and native buffer lifetime defects in the
[area A audit](cppgw-lifetime-a-20261118.md). The integration parent is
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06` and the audit-first merge is
`72242018d0605061c8659961cff69d1a2daa3467`. Each implementation commit covers
one defect or a closely related family. The table identifies the commits that
recorded the fixes and the deferred design.

Owned surfaces are the assigned runtime modules under `src/rt` and
`include/uos/rt`, their existing or registered tests, and this report. TypeScript,
workflows, other C++ areas, dependency version changes and deployments are outside
this work. Run local fixtures in the CI driver's loopback-only namespaces with
empty homes, fresh temporary directories and a clean environment.

| Behavior surface                     | Invariant                                                                                                      |
| ------------------------------------ | -------------------------------------------------------------------------------------------------------------- |
| Abort, Promise, generator and stream | Retain synchronous listener ordering, microtask timing, terminal reasons and fail-fast combinator behavior.    |
| Fetch and WebSocket                  | Keep request and response bytes, headers, status and close codes, cancellation and delivery semantics.         |
| Production routing and accounting    | Keep the existing settlement, storage and provider selection contracts. Exercise mock and loopback paths only. |

Check each site against the merged sources before editing it. Where possible,
exercise its failing interleaving with a regression and run the affected fixture
via `bash cpp/gateway/tools/ci-build.sh --config release <test>` and
`bash cpp/gateway/tools/ci-build.sh --config asan <test>`. The driver includes all
runtime objects in every test, so final verification uses four full sanitizer
shards, one full release shard and `python3 cpp/gateway/tools/links.py --check`.
Cold builds and sanitizer coverage are initially unverified. Prepare the pinned
dependencies in `/var/tmp/cppgw-lifetime-fix-a-20261118`, run bounded driver
commands with incremental objects, and preserve any failed receipts. Stop when
each of 29 defect sites has a current-tree verdict and final verification
completes, or record the precise pending work and last passing tree.

## Defect verdicts

| Audit ID and site                                          | Verdict and reason                                                                                                                                                                                                                                                                                                                                                                                             | Commit                    | Regression or coverage                                                                                                                                                                                    |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A-ABORT-OWNER (`src/rt/abort.cpp`)                         | FIXED. `AbortController::abort` passed its member slot by reference into `signalAbort`. A listener could destroy that slot before the next once listener and final cleanup. Dispatch now owns the original signal.                                                                                                                                                                                             | `0176c8e8`                | `rt/abort_test`, a listener destroys the only controller.                                                                                                                                                 |
| A-LOOP-TEARDOWN-OWNER (`src/rt/async.cpp`)                 | FIXED. Frame destructors could post under the ambient loop after the dying loop's run scope ended. Run destruction under the dying loop and discard work submitted to it during shutdown, including dispatch that would otherwise run inline.                                                                                                                                                                  | `427e2763`                | `rt/async_test`, a destroyed frame queues a microtask while another loop is in scope. Baseline leaks the job into that loop.                                                                              |
| A-PROMISE-REACTION-CYCLE (`src/rt/async.cpp`)              | LEFT. The void `all` reaction still captures its own pending source. The intrusive list releases the cycle at scoped shutdown, which never runs for the process loop. Moving the owner to a queued delivery requires the coordinated change below.                                                                                                                                                             | `6a998c23`                | Static owner trace through `all`, `react` and `settle`.                                                                                                                                                   |
| A-PROMISE-GCC-AWAIT (`include/uos/rt/async.hpp`)           | FIXED. `awaitOwned` and `awaitState` implicitly constructed an owning base pointer in the await full-expression. Create the Task first.                                                                                                                                                                                                                                                                        | `6567a6c8`                | Existing Promise await and turn-count cases in `rt/async_test`. The GCC 12 rule is checked by inspection.                                                                                                 |
| A-PROMISE-CALLBACK-INPUT (`include/uos/rt/async.hpp`)      | FIXED. The reaction was the last owner of the outcome passed by reference to a Task-returning callback. Its child now owns both the callable and the outcome through completion. Value and Promise callbacks use the same settlement path as before.                                                                                                                                                           | `22222e39`                | `rt/async_test`, a pending sibling reads the value after early rejection, and `then` and `catch_` read rejection reasons after suspension.                                                                |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/async.hpp`)      | LEFT. Typed adoption, callback registration and combinators have the same backedge. Tuple `all` additionally retains all input Promises in each reaction. A change at one site is insufficient to establish ownership of the combinators.                                                                                                                                                                      | `6a998c23`                | Static owner trace. Existing `rt/async_test` settles inputs or runs under a scoped loop.                                                                                                                  |
| A-GENERATOR-STEP-OWNER (`include/uos/rt/async.hpp`)        | FIXED. `requestStep` eagerly enqueues the result, but the generator body deliberately keeps only a weak link. Keep the core in the returned Task so either step can still settle after the last generator handle is dropped.                                                                                                                                                                                   | `4b03b177`                | `rt/async_test`, checks `next` and a queued `return_` before a timer. The baseline failure is liveness.                                                                                                   |
| A-GENERATOR-GCC-AWAIT (`include/uos/rt/async.hpp`)         | FIXED. `delegateTo` and `forAwait` materialized owning yielded arguments in await expressions. Name the Tasks. The same review found that `runGenerator` erased its Yield argument by value. Forward the frame's own Yield instead so reference-taking bodies can use it after suspension.                                                                                                                     | `3848b5f1`                | Existing delegation and iteration tests plus a borrowed Yield regression in `rt/async_test`. GCC 12 expression shapes were checked by inspection.                                                         |
| A-PROCESS-SIGNAL-CONTEXT (`src/rt/process_loop.cpp`)       | FIXED. The static signal set could retain the first scoped Asio context past destruction. An Asio service for each loop now owns that loop's listeners and destroys its signal set at shutdown.                                                                                                                                                                                                                | `1cfb9d83`                | `rt/process_test`, leaves an armed registration at teardown, checks the default action, then removes the identity in a new loop.                                                                          |
| A-STREAM-GCC-AWAIT (`include/uos/rt/stream.hpp`)           | FIXED. `runAsync` and `iterateStream` now name the Tasks before awaiting. The async erasure forwards a reference to the argument owned by `runAsync`.                                                                                                                                                                                                                                                          | `448ad930`                | `rt/stream_test`, a pull Task borrows its controller across suspension. Existing stream iteration covers named yields.                                                                                    |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/stream.hpp`)     | LEFT. Stream start, pull, cancel and tee-closed reactions still capture their own source Promise. The weak tee change removes the source-stream retention without redesigning these registrations.                                                                                                                                                                                                             | `6a998c23`                | Static owner trace. `rt/stream_test` checks the separate abandoned-tee defect.                                                                                                                            |
| A-STREAM-CANCEL-OWNER (`include/uos/rt/stream.hpp`)        | FIXED. `cancelStreamInto` read the reader's stream slot after the synchronous cancel algorithm could empty it through `releaseLock`. Take an owning copy before closing the stream or invoking the callback.                                                                                                                                                                                                   | `f9eb14af`                | `rt/stream_test`, cancel releases the reader while a read is pending.                                                                                                                                     |
| A-STREAM-TEE-READ-CYCLE (`include/uos/rt/stream.hpp`)      | FIXED. `teeRead` held the reader and its source across an await on that source's own request. Let the branch algorithms own the tee, and use a weak link while waiting. The async algorithm wrapper already owns the cancellation callable across its await, so an active cancellation still owns the tee. The original weak fix passed the follow-up cancel replay without a separate cancellation owner.     | `5a54034d` and `04dc8168` | `rt/stream_test`, checks reclamation after dropping both branches and confirms a surviving cancellation settles on source close and error. A parked Promise awaiter can still remain until loop shutdown. |
| A-PROMISE-GCC-AWAIT (`include/uos/rt/stream.hpp`)          | FIXED. `teeCancel` also converted the typed state into an owning base pointer inside its await. Use a named Task.                                                                                                                                                                                                                                                                                              | `6567a6c8`                | Existing combined cancellation and reason cases in `rt/stream_test`. The GCC 12 rule is checked by inspection.                                                                                            |
| A-FETCH-CALLBACK-INPUT (`include/uos/rt/fetch.hpp`)        | FIXED. A reference-taking coroutine passes the constructor constraint, but the erased by-value parameters end before the Task starts. Forward references to owned argument storage through the erasure.                                                                                                                                                                                                        | `d0420556`                | `rt/fetch_test`, a callback uses both borrowed inputs after a microtask.                                                                                                                                  |
| A-FETCH-ABORT-BODY-CYCLE (`src/rt/fetch.cpp`)              | FIXED. An unread HWM-zero wrapper could never reach a callback that broke its strong controller backedge. The abort listener now promotes a weak wrapper link, and wrapper destruction removes the listener.                                                                                                                                                                                                   | `cac9928f`                | `rt/fetch_test`, drops an unread response while its signal survives and checks both streams expire.                                                                                                       |
| A-FETCH-ABORT-TRANSPORT (`src/rt/fetch.cpp`)               | FIXED. `sendThrough` could reject on abort while a replaced transport's virtual coroutine remained suspended on an rt Promise. Give its child Task the transport owner before calling `abortable`. The front end still rejects immediately.                                                                                                                                                                    | `8e6b2404`                | `rt/fetch_test`, replaces the transport, aborts the fetch and resumes the child separately.                                                                                                               |
| A-FETCH-REENTRANT-FUNCTION (`src/rt/fetch.cpp`)            | FIXED. Argument evaluation permitted callback invocation before the callable owner was copied. Snapshot the owner before invocation.                                                                                                                                                                                                                                                                           | `d0420556`                | `rt/fetch_test`, a synchronous callback replaces the global function and reads its capture. The baseline depends on argument evaluation order.                                                            |
| A-FETCH-CALLBACK-INPUT (`src/rt/fetch.cpp`)                | FIXED. The old wrapper pinned only the function. The returned Task also retains the original request arguments through completion.                                                                                                                                                                                                                                                                             | `d0420556`                | `rt/fetch_test`, same borrowed-input regression as the header site.                                                                                                                                       |
| A-MOCK-HANDLER-OWNER (`src/rt/fetch.cpp`)                  | FIXED. `send` recorded a request handle but returned the handler's bare Task. Neither its closure nor the actual argument slot had to survive. Preserve both in the returned Task.                                                                                                                                                                                                                             | `f6c9c315`                | `rt/fetch_test`, destroys the mock before awaiting a capturing, reference-taking handler.                                                                                                                 |
| A-FETCH-UPLOAD-STRANDED (`src/rt/fetch_transport.cpp`)     | FIXED. The integrated writer still parks on a stream Promise which socket close cannot wake. The active upload registers a one-shot close hook with a weak writer link. It releases the reader on connection close, keeping the original abort reason when applicable and discarding the synthetic reader-release rejection. The writer removes the hook on exit so pooled connections retain no upload state. | `52d6f720`                | `rt/fetch_transport_test`, production transport against loopback. Separately exercises EOF before headers and an early 413 while the upload read is parked.                                               |
| A-FETCH-RESPONSE-STRANDED (`src/rt/fetch_transport.cpp`)   | FIXED. The integrated parser still had a silent return for an aborted read when the pump owned a connection. Preserve the successful explicit-cancel return when the connection slot has been cleared. Otherwise reject with the recorded upload reason, the existing timeout, or the existing closed-socket error.                                                                                            | `d61ed783`                | `rt/fetch_transport_test`, 200 headers arrive, then a parked response read rejects with the original upload reason.                                                                                       |
| A-FETCH-NET-GCC-AWAIT (`src/rt/fetch_transport_net.cpp`)   | FIXED as a portability rule. The inline Asio descriptors are non-owning and the request frame or TLS flush frame already pins the bytes. Name the five buffer descriptors before awaiting.                                                                                                                                                                                                                     | `d77b297a`                | Existing plain and TLS fixtures in `rt/fetch_transport_test`. The GCC 12 expression-shape check is static.                                                                                                |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket.cpp`)               | FIXED. An external WebSocket owner could retain the Asio socket after its service was destroyed, even after close delivery. Detach the native state during registry shutdown and remember its buffered count.                                                                                                                                                                                                  | `dbe4beac`                | `rt/websocket_lifetime_test`, destroys the loop before an open and an already closed WebSocket.                                                                                                           |
| A-WEBSOCKET-EMIT-OWNER (`src/rt/websocket.cpp`)            | FIXED. A snapshot retains listener functions but not the receiver needed to remove a later once registration. Public emit pins shared WebSocket and server receivers for that dispatch.                                                                                                                                                                                                                        | `c37174cb`                | `rt/websocket_lifetime_test`, first listener releases the only handle before once removal.                                                                                                                |
| A-WEBSOCKET-UPGRADE-OWNER (`src/rt/websocket.cpp`)         | FIXED. `completeUpgrade` reused server options and client tracking after calling injected hooks. Pin the server across the whole call, including the factory before any event is emitted.                                                                                                                                                                                                                      | `c8e672ba`                | `rt/websocket_lifetime_test`, factory drops the last server handle before it uses the supplied options and completes the upgrade.                                                                         |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_internal.hpp`) | FIXED. The pending entry owned the only copy of native plaintext or TLS ciphertext. `onWritten` now takes back the completion's owned bytes when the queue is still active.                                                                                                                                                                                                                                    | `4bec40c0`                | `rt/websocket_lifetime_test`, forced backpressure, partial writes and cancellation.                                                                                                                       |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket_internal.hpp`)      | FIXED. Both `State::socket` and `State::sender` retained the same loop-bound transport. Registry shutdown now releases both without emitting close.                                                                                                                                                                                                                                                            | `dbe4beac`                | Same retained-handle regression.                                                                                                                                                                          |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_socket.cpp`)   | FIXED. `destroy` and `shutdown` clear the write queue before Asio releases its operation. Move the byte storage into the completion handler on the would-block path.                                                                                                                                                                                                                                           | `4bec40c0`                | `rt/websocket_lifetime_test`. Linux's reactor cancels the wait before a userspace buffer read, so ordinary ASan cannot deterministically diagnose the baseline contract violation.                        |

### Deferred Promise reaction design

Give outcome-consuming reactions a typed entry point that receives the settled
state at delivery. Pending jobs then hold targets and callables without owning
their own source. `PromiseCore::react` for an already settled source and
`PromiseCore::settle` for a pending source would attach an owner to the queued
microtask. A callback that returns a Task must transfer that settled source
owner into its child frame, as this branch already does for `then` and `catch_`.
`finally_` must also keep the original outcome through asynchronous cleanup.

Migrate adoption, `then`, `catch_`, `finally_`, the vector and tuple
combinators, and the four stream registrations together. When each tuple input
fulfills, keep its value in an indexed slot and assemble the tuple on the last
fulfillment. Holding the tuple of source Promises would reintroduce the backedge.
Keep FIFO reaction delivery, rejection handling and the existing adoption
microtask count. A resolverless losing input should expire while its loop remains
live. A losing input with a surviving resolver should still run its own callbacks
when it settles. Test both cases with weak identities, including a mixed tuple
with void slots, then run the whole suite under sanitizers.

This handoff does not collect a never-settled Promise with a suspended Task
awaiter. The Asio resume handler and the awaiting frame own one another through
the Promise. Removing that cycle without invalidating externally awaited Tasks
requires a distinct abandonment mechanism. This pass makes concrete production
operations settle when their connection closes.

## UNCLEAR sites

| Site                           | Question from audit                                                 | Verdict                                                                                                                                                                                                                                                                                                                                                                              |
| ------------------------------ | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/rt/clock.cpp`             | Self-replacement of injected global hooks                           | LEFT. Direct invocation borrows the installed `std::function`. A capturing override can replace its own target, but production uses the defaults. A simple copy before invoking would change mutable override state across calls.                                                                                                                                                    |
| `src/rt/async.cpp`             | Parked awaiter and pending Promise ownership                        | LEFT. `awaitSettled` owns its input state and the reaction owns the continuation. The loop teardown contract explicitly permits this for unresolved Tasks. The detached Fetch upload now has a concrete wakeup on connection close.                                                                                                                                                  |
| `include/uos/rt/stream.hpp`    | Borrowing callback parameters through the by-value erased signature | FIXED with A-STREAM-GCC-AWAIT. A reference-taking coroutine used to point into the by-value function invocation. Forward the frame-owned argument.                                                                                                                                                                                                                                   |
| `src/rt/http.cpp`              | Abandoned streamed-body reader lock                                 | FIXED for a started read rejecting or unwinding. A Bun 1.4.0 probe gave `locked=false` after rejection and after success. `finishConsume` now releases on either path, covered by `rt/http_test`. LEFT for a Task discarded before start. Bun consumes eagerly even if its Promise is ignored. Matching that behavior for dropped lazy Tasks requires a separate ownership decision. |
| `include/uos/rt/websocket.hpp` | Self-replacement of injected buffered-amount getter                 | LEFT. The public mutable `std::function&` override can destroy its active target. Preserving stateful getters would require changing the override registration contract.                                                                                                                                                                                                             |
| `src/rt/websocket.cpp`         | Injected buffered-amount getter replaces itself                     | LEFT. `bufferedAmount` calls the live slot directly. The fixed production getter has no replacement path. Same test seam as the header site.                                                                                                                                                                                                                                         |
| `src/rt/websocket_client.cpp`  | Unfinished ownership after a handled unexpected response            | NOT A DEFECT under the documented client contract. A handled response leaves the WebSocket CONNECTING until termination. Peer close releases request listeners and loop teardown shuts down the socket. The live sideband's finish path terminates the opening upstream socket, after mapping retirement for 404 or 410.                                                             |

## Verification

The code changes touch the async, abort, stream, HTTP, Fetch, process signal and
WebSocket runtime modules and their existing test binaries. The driver links
every runtime source into every test. Focused checks use the affected runtime
fixtures against their normal APIs. The final selection is the complete ASan
inventory plus release shard 1 and the link registry check.

The runner is Linux ARM64 with Clang 21.1.8, Boost 1.92.0 static JSON and
Container, ICU 78.3, SQLite 3.54.0 with `SQLITE_DQS=3`, OpenSSL 3.0.13,
zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. Dependency preparation used the pinned
archives and SHA-256s in the integration report, staged outside the checkout.
The driver proved loopback access, outside-route denial and the staged SQLite
version from inside the fixture namespace. [Dependency receipts](cppgw-integrate-f2-20261118.md#runner-and-dependencies)

During iteration, the abort, async and stream binaries passed 59 cases in each
profile. The first WebSocket and Fetch test attempts exposed a missing local
socket header and an ambiguous unqualified `sleep`. Those fixture errors were
corrected and folded into their respective fix commits. The
WebSocket write and Fetch front-end fixtures then passed 19 cases in each
profile. A later focused run on `d77b297a3dc6ae86a6bd442f215fbba56907bed3`
passed all 9 selected binaries and 150/150 cases in each profile. Release took
432.541 s and ASan/UBSan/LSan took 589.796 s. These are incremental focused
measurements with one worker competing with cold production-object builds.
The follow-up stream and HTTP selection passed both binaries and 21/21 cases in
each profile on `dbf15da3bad8fceeb3ebf44a885eec49a3afb653`. Release took
99.216 s and ASan/UBSan/LSan took 129.572 s.

The driver starts each binary in fresh Linux network, mount and PID namespaces.
Its allowlisted fixture environment and private home and temporary directory also
apply to the baseline controls. The outer launch uses the following configuration.
Here `$scratch` is `/var/tmp/cppgw-lifetime-fix-a-20261118` and `$config` is
`release` or `asan`.

```bash
session=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
instrument=
if [[ "$config" == asan ]]; then
  instrument="-gline-tables-only -fno-sanitize-recover=all"
fi
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$config" --shard "$shard/4"
```

The focused gates ran the same driver, limiting its object inventory to runtime
and resolved test closures in an isolated worktree, with these selections and a
separate reusable build directory.

```bash
bash "$scratch/run-gate.sh" "$config" 1 "$scratch/focus" "$scratch/focus-build" \
  python3 -B "$scratch/focused.py" "$config" \
  rt/abort rt/async rt/stream rt/process rt/fetch rt/fetch_connection rt/fetch_transport rt/websocket_lifetime rt/websocket
bash "$scratch/run-gate.sh" "$config" 1 "$scratch/focus" "$scratch/focus-build" \
  python3 -B "$scratch/focused.py" "$config" rt/stream rt/http
```

### Baseline controls

The replay worktree uses baseline production source from the audit merge and
the candidate fixtures in commit `73e60c6c93763284046f2e6320780536c90409f2`,
tree `a99cc13d6060349677e0f975d3efc63c25c2abb3`. The scratch selector forwards
one case-name substring per test binary through the standard fixture boundary.
The command below ran five successive selections. Driver wall times were
646.290 s for the cold build and first set, then 5.954 s, 4.495 s, 3.866 s and
1.173 s using those objects.

```bash
bash "$scratch/run-baseline.sh"
```

| Baseline regression                                      | Result on Clang 21.1.8 with ASan, UBSan and LSan                        |
| -------------------------------------------------------- | ----------------------------------------------------------------------- |
| Abort dispatch after the last controller is destroyed    | ASan heap-use-after-free                                                |
| Cancellation after a callback releases the active reader | UBSan null member call                                                  |
| Pending generator next and return                        | The step liveness assertion failed                                      |
| Promise callback borrowing a settled outcome             | ASan heap-use-after-free                                                |
| Loop teardown under an ambient loop                      | The queued job reaches the outer loop, caught by assertions             |
| Process signal set retained across loops                 | ASan heap-use-after-free                                                |
| Borrowed Fetch inputs and mock callback arguments        | ASan stack-use-after-return in each case                                |
| Aborted Fetch with a suspended transport                 | ASan heap-use-after-free                                                |
| Unread abortable Fetch response                          | LSan reported 3,640 bytes in 22 allocations                             |
| Parked upload after EOF and early response               | Weak owner assertions failed                                            |
| Response read after post-header upload failure           | The read stayed pending                                                 |
| Abandoned tee with a parked read                         | The original stream stayed alive                                        |
| Rejected HTTP body read                                  | The original stream stayed locked                                       |
| Retained WebSocket after loop teardown                   | The closed Asio socket stayed alive, caught by weak owner assertions    |
| Public WebSocket emit after the last owner is dropped    | ASan heap-use-after-free                                                |
| Reentrant server upgrade                                 | Owner assertion failed and LSan reported 67,976 bytes in 12 allocations |

Together the 21 controls yielded 17 expected failures. The backpressured
socket control passed on baseline because canceling the Linux reactor operation
does not force a dereference of the freed data. The two borrowed stream and
generator arguments passed on this compiler in their old await
full-expressions. The Fetch replacement case also passed on Clang and on the
GCC 14.2 sanitizer comparison. C++ permits a different argument evaluation
order, and the changed code pins the callable before invocation in either
order. The GCC comparison used `CC=gcc-14 CXX=g++-14`, `-g1` and the staged
ICU headers as system headers. With ICU passed as a project include, the first
GCC attempt stopped on an upstream `unistr.h` conversion warning. The retry
compiled 44 runtime objects and passed its one selected case in 485.064 s.

```bash
bash "$scratch/run-gcc-gate.sh" asan 1 "$scratch/red" "$scratch/red-gcc-build" \
  python3 -B "$scratch/baseline.py" asan 'rt/fetch:replace itself during'
```

The tee cancellation replay on `e7610eaa` passed with only the original weak
read change. This confirmed the cancel algorithm's existing owner is enough
and prompted removal of the redundant follow-up source changes. The branch
retains the regression test. The interrupted full builds from before that
simplification are excluded from the final gate results.

### Final gates

All five ordinary `ci-build.sh` runs used candidate
`8f972f3e6d25c06b74616e9aa606642fc957e323`, tree
`bc89c308a6b4b7a05f6c8d047fcc1ba295de11f8`. The input digest before and after
each gate was `9cac5c2b3b25ac9ba6378f75f736cc9abfd928c46407da6e1ebbd136078b62df`.
Every run compiled or reused all 369 canonical production objects.

```bash
bash "$scratch/run-official.sh" release 8f972f3e6d25c06b74616e9aa606642fc957e323 1
bash "$scratch/run-official.sh" asan 8f972f3e6d25c06b74616e9aa606642fc957e323 1 2 3 4
python3 -B "$scratch/verify-final.py" 8f972f3e6d25c06b74616e9aa606642fc957e323
```

Release used two workers, sanitizer shard 1 used three, and the remaining
sanitizer shards used four. The first release and sanitizer runs overlapped.
Each command below exited 0. The times include rebuilds from the existing
object cache.

| Gate                | Passing binaries |   Passing cases | Compile (s) | Link and test (s) | Driver wall (s) |
| ------------------- | ---------------: | --------------: | ----------: | ----------------: | --------------: |
| Release 1/4         |          113/113 |     1,337/1,337 |   1,959.829 |            57.250 |       2,017.602 |
| ASan 1/4            |          113/113 |     1,337/1,337 |   1,778.034 |           114.588 |       1,893.144 |
| ASan 2/4            |          113/113 |     1,246/1,246 |     380.109 |            35.509 |         416.146 |
| ASan 3/4            |          112/112 |     1,446/1,446 |     338.682 |            38.337 |         377.548 |
| ASan 4/4            |          112/112 |     1,106/1,106 |     311.700 |            99.808 |         412.021 |
| **Sanitizer total** |      **450/450** | **5,135/5,135** |             |                   |                 |

The sanitizer selections partitioned the full 450-binary registry. All 450
run logs recorded the namespace boundary and showed zero ASan, UBSan or LSan
diagnostics. The hashes below identify the recorded receipts in `$scratch/final`.

| Receipt          | SHA-256                                                            |
| ---------------- | ------------------------------------------------------------------ |
| `release-1.json` | `cceccee15374a3cd27a923246afa19c5a3c7871af5994c68326d5fcda2b97962` |
| `asan-1.json`    | `281a3fe6a4706b3b5f2aff304e63d221a8711d5efcf4fb02443e18b92f7d8619` |
| `asan-2.json`    | `81a31754e5ce0c5559270d134a371a447aa6022f2a3346d3970ef584122c9926` |
| `asan-3.json`    | `ba99a5d1120b2bbd7c2c9819ad9318c6db2bba02388a34fc50f7e5c86dc9b385` |
| `asan-4.json`    | `f660a4667ecd77e97cc597006f366545e765148b6e5c9bbc1ffd7b4295b57459` |

The exact candidate also passed all 10 focused runtime binaries and 158/158
cases in each profile. Release took 90.653 s and sanitizers took 138.120 s.
These focused checks used the receipt directory for the isolated worktree.

```bash
bash "$scratch/run-gate.sh" "$config" 1 "$scratch/focus" "$scratch/focus-build" \
  python3 -B "$scratch/focused.py" "$config" \
  rt/abort rt/async rt/stream rt/http rt/process rt/fetch rt/fetch_connection rt/fetch_transport rt/websocket_lifetime rt/websocket
```

`PYTHONDONTWRITEBYTECODE=1 python3 cpp/gateway/tools/links.py --check` resolved all
450 registered binaries. `git diff --check aae39d0e..HEAD` passed. The final
report commit adds receipts and findings text to the tested candidate.

No confirmed out-of-area lifetime defects were found in the caller paths
reviewed for these changes.

A GitHub push dry run returned `fatal: unable to get password from user`.
The local commits and this PR body are the handoff for the job's branch
publisher.

## Summary

Fixed 26 audited lifetime sites that could crash the gateway, leave requests
hanging, or keep resources alive after cancellation. Callbacks and socket
writes now keep their inputs alive through completion, and closing a connection
wakes parked uploads and response reads.

All 450 sanitizer binaries passed on Linux ARM64, covering 5,135 cases. The
full release shard passed 113 binaries, and the link check passed. The three
remaining audit rows cover one pending-Promise retention issue that needs an
ownership change across the Promise and stream code.

The commits are ready for the PR handoff. GitHub write access was unavailable
in this checkout.
