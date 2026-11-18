# Native gateway runtime lifetime fixes, area A

## Work contract v1

Close the confirmed coroutine, callback and native buffer lifetime defects in the
[area A audit](cppgw-lifetime-a-20261118.md). The integration parent is
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06` and the audit-first merge is
`72242018d0605061c8659961cff69d1a2daa3467`. Each commit covers one defect or closely
related family. A row saying “this commit” refers to the Git commit that last
changed the verdict in that row. Run `git log -p -- <this report>` to recover it.

Owned surfaces are the assigned runtime modules under `src/rt` and
`include/uos/rt`, their existing or registered tests, and this report. TypeScript,
workflows, other C++ areas, dependency version changes and deployments are outside
this work. Run local fixtures in the CI driver's loopback-only namespaces with
empty homes, fresh temporary directories and a clean environment.

| Behavior surface | Invariant |
| --- | --- |
| Abort, Promise, generator and stream | Retain synchronous listener ordering, microtask timing, terminal reasons and fail-fast combinator behavior. |
| Fetch and WebSocket | Keep request and response bytes, headers, status and close codes, cancellation and delivery semantics. |
| Production routing and accounting | Keep the existing settlement, storage and provider selection contracts. Exercise mock and loopback paths only. |

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

| Audit ID and site | Verdict and reason | Commit | Regression or coverage |
| --- | --- | --- | --- |
| A-ABORT-OWNER (`src/rt/abort.cpp`) | FIXED. `AbortController::abort` passed its member slot by reference into `signalAbort`. A listener could destroy that slot before the next once listener and final cleanup. Dispatch now owns the original signal. | This commit | `rt/abort_test`, a listener destroys the only controller. |
| A-LOOP-TEARDOWN-OWNER (`src/rt/async.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-REACTION-CYCLE (`src/rt/async.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-GCC-AWAIT (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-CALLBACK-INPUT (`include/uos/rt/async.hpp`) | FIXED. The reaction was the last owner of the outcome passed by reference to a Task-returning callback. Its child now owns both the callable and the outcome through completion. Value and Promise callbacks use the same settlement path as before. | This commit | `rt/async_test`, a pending sibling reads the value after early rejection, and `then` and `catch_` read rejection reasons after suspension. |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-GENERATOR-STEP-OWNER (`include/uos/rt/async.hpp`) | FIXED. `requestStep` eagerly enqueues the result, but the generator body deliberately keeps only a weak link. Keep the core in the returned Task so either step can still settle after the last generator handle is dropped. | This commit | `rt/async_test`, checks `next` and a queued `return_` before a timer. The baseline failure is liveness. |
| A-GENERATOR-GCC-AWAIT (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROCESS-SIGNAL-CONTEXT (`src/rt/process_loop.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-STREAM-GCC-AWAIT (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-STREAM-CANCEL-OWNER (`include/uos/rt/stream.hpp`) | FIXED. `cancelStreamInto` read the reader's stream slot after the synchronous cancel algorithm could empty it through `releaseLock`. Take an owning copy before closing the stream or invoking the callback. | This commit | `rt/stream_test`, cancel releases the reader while a read is pending. |
| A-STREAM-TEE-READ-CYCLE (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-GCC-AWAIT (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-CALLBACK-INPUT (`include/uos/rt/fetch.hpp`) | FIXED. A reference-taking coroutine passes the constructor constraint, but the erased by-value parameters end before the Task starts. Forward references to owned argument storage through the erasure. | This commit | `rt/fetch_test`, a callback uses both borrowed inputs after a microtask. |
| A-FETCH-ABORT-BODY-CYCLE (`src/rt/fetch.cpp`) | FIXED. An unread HWM-zero wrapper could never reach a callback that broke its strong controller backedge. The abort listener now promotes a weak wrapper link, and wrapper destruction removes the listener. | This commit | `rt/fetch_test`, drops an unread response while its signal survives and checks both streams expire. |
| A-FETCH-ABORT-TRANSPORT (`src/rt/fetch.cpp`) | FIXED. `sendThrough` could reject on abort while a replaced transport's virtual coroutine remained suspended on an rt Promise. Give its child Task the transport owner before calling `abortable`. The front end still rejects immediately. | This commit | `rt/fetch_test`, replaces the transport, aborts the fetch and resumes the child separately. |
| A-FETCH-REENTRANT-FUNCTION (`src/rt/fetch.cpp`) | FIXED. Argument evaluation permitted callback invocation before the callable owner was copied. Snapshot the owner before invocation. | This commit | `rt/fetch_test`, a synchronous callback replaces the global function and reads its capture. The baseline depends on argument evaluation order. |
| A-FETCH-CALLBACK-INPUT (`src/rt/fetch.cpp`) | FIXED. The old wrapper pinned only the function. The returned Task also retains the original request arguments through completion. | This commit | `rt/fetch_test`, same borrowed-input regression as the header site. |
| A-MOCK-HANDLER-OWNER (`src/rt/fetch.cpp`) | FIXED. `send` recorded a request handle but returned the handler's bare Task. Neither its closure nor the actual argument slot had to survive. Preserve both in the returned Task. | This commit | `rt/fetch_test`, destroys the mock before awaiting a capturing, reference-taking handler. |
| A-FETCH-UPLOAD-STRANDED (`src/rt/fetch_transport.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-RESPONSE-STRANDED (`src/rt/fetch_transport.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-NET-GCC-AWAIT (`src/rt/fetch_transport_net.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-EMIT-OWNER (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-UPGRADE-OWNER (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_internal.hpp`) | FIXED. The pending entry owned the only copy of native plaintext or TLS ciphertext. `onWritten` now takes back the completion's owned bytes when the queue is still active. | This commit | `rt/websocket_lifetime_test`, forced backpressure, partial writes and cancellation. |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket_internal.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_socket.cpp`) | FIXED. `destroy` and `shutdown` clear the write queue before Asio releases its operation. Move the byte storage into the completion handler on the would-block path. | This commit | `rt/websocket_lifetime_test`. Linux's reactor cancels the wait before a userspace buffer read, so ordinary ASan cannot deterministically diagnose the baseline contract violation. |

## UNCLEAR sites

| Site | Question from audit | Verdict |
| --- | --- | --- |
| `src/rt/clock.cpp` | Self-replacement of injected global hooks | LEFT. Current-tree re-verification pending. |
| `src/rt/async.cpp` | Parked awaiter and pending Promise ownership | LEFT. Current-tree re-verification pending. |
| `include/uos/rt/stream.hpp` | Borrowing callback parameters through the by-value erased signature | LEFT. Current-tree re-verification pending. |
| `src/rt/http.cpp` | Abandoned streamed-body reader lock | LEFT. Current-tree re-verification pending. |
| `include/uos/rt/websocket.hpp` | Self-replacement of injected buffered-amount getter | LEFT. Current-tree re-verification pending. |
| `src/rt/websocket.cpp` | Injected buffered-amount getter replaces itself | LEFT. Current-tree re-verification pending. |
| `src/rt/websocket_client.cpp` | Unfinished ownership after a handled unexpected response | LEFT. Current-tree re-verification pending. |

## Verification

Pending toolchain and dependency setup. No check has run on the fix branch yet.

## Summary

The audit has been merged and the verdicts are pending current-tree verification.
