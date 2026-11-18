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
| A-PROMISE-CALLBACK-INPUT (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-GENERATOR-STEP-OWNER (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-GENERATOR-GCC-AWAIT (`include/uos/rt/async.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROCESS-SIGNAL-CONTEXT (`src/rt/process_loop.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-STREAM-GCC-AWAIT (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-REACTION-CYCLE (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-STREAM-CANCEL-OWNER (`include/uos/rt/stream.hpp`) | FIXED. `cancelStreamInto` read the reader's stream slot after the synchronous cancel algorithm could empty it through `releaseLock`. Take an owning copy before closing the stream or invoking the callback. | This commit | `rt/stream_test`, cancel releases the reader while a read is pending. |
| A-STREAM-TEE-READ-CYCLE (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-PROMISE-GCC-AWAIT (`include/uos/rt/stream.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-CALLBACK-INPUT (`include/uos/rt/fetch.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-ABORT-BODY-CYCLE (`src/rt/fetch.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-ABORT-TRANSPORT (`src/rt/fetch.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-REENTRANT-FUNCTION (`src/rt/fetch.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-CALLBACK-INPUT (`src/rt/fetch.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-MOCK-HANDLER-OWNER (`src/rt/fetch.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-UPLOAD-STRANDED (`src/rt/fetch_transport.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-RESPONSE-STRANDED (`src/rt/fetch_transport.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-FETCH-NET-GCC-AWAIT (`src/rt/fetch_transport_net.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-EMIT-OWNER (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-UPGRADE-OWNER (`src/rt/websocket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_internal.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-CONTEXT (`src/rt/websocket_internal.hpp`) | LEFT. Current-tree re-verification pending. | — | — |
| A-WEBSOCKET-WRITE-BUFFER (`src/rt/websocket_socket.cpp`) | LEFT. Current-tree re-verification pending. | — | — |

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
