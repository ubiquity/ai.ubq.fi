# C++ gateway coroutine and callback lifetime audit, area A

## Audit contract v1

Review the runtime network layer for coroutine closure, borrowed parameter, callback
ownership, container invalidation and GCC 12 await-expression hazards. Analyze source
starting at `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`,
which is the supplied integration branch tip. Paths and line numbers below are
relative to `cpp/gateway/` on that tree.

The owned artifacts are this report and `analysis/lifetime/a/`. Production code, tests,
TypeScript, workflows and other audit areas are out of scope for edits. Preserve the
runtime's fail-fast and race behavior, stream and socket cancellation, callback delivery
and network/accounting contracts. Follow callers across the boundary only to establish
ownership and record any findings there for the assigned owners.

The audit matrix covers overloaded and template entry points, native and fallback
branches, and ordinary, early-return, cancellation and teardown paths. Static review
covers all tracked files matching the area patterns and the ProcessLoop public
declarations in `process.hpp`. Dynamic checks use Linux ARM64, local-only fixtures
and a clean environment with an empty home and fresh temporary directory. No provider
connections or live deployment are in scope.

Use `rg` and the five requested clang-tidy checks to discover sites. Assign a verdict
from the actual owner/caller path, record it in both artifacts and commit each finished
file. Verify the report/JSON correspondence, formatting and `git diff --check`; run
bounded sanitizer checks via `bash cpp/gateway/tools/ci-build.sh --config asan --shard N/4`
or focused local fixtures where feasible. Toolchain availability, analyzer coverage
and the duration of sanitizer builds are initially unverified. Setup is capped at
20 minutes; source review continues if a tool cannot complete. Stop after all files,
sites and ranked proposed fixes reconcile, or report exactly what remains.

## Checklist (30 files)

- [ ] `src/rt/abort.cpp`
- [ ] `include/uos/rt/abort.hpp`
- [ ] `src/rt/clock.cpp`
- [ ] `include/uos/rt/clock.hpp`
- [ ] `src/rt/async.cpp`
- [ ] `include/uos/rt/async.hpp`
- [ ] `src/rt/process_loop.cpp`
- [ ] `include/uos/rt/process.hpp`
- [ ] `src/rt/stream.cpp`
- [ ] `include/uos/rt/stream.hpp`
- [ ] `src/rt/http.cpp`
- [ ] `include/uos/rt/http.hpp`
- [ ] `include/uos/rt/fetch.hpp`
- [ ] `include/uos/rt/fetch_transport.hpp`
- [ ] `include/uos/rt/gzip.hpp`
- [ ] `include/uos/rt/websocket.hpp`
- [ ] `src/rt/fetch.cpp`
- [ ] `src/rt/fetch_transport.cpp`
- [ ] `src/rt/fetch_transport_codec.cpp`
- [ ] `src/rt/fetch_transport_http.cpp`
- [ ] `src/rt/fetch_transport_internal.hpp`
- [ ] `src/rt/fetch_transport_net.cpp`
- [ ] `src/rt/gzip.cpp`
- [ ] `src/rt/tls.cpp`
- [ ] `src/rt/tls_internal.hpp`
- [ ] `src/rt/websocket.cpp`
- [ ] `src/rt/websocket_client.cpp`
- [ ] `src/rt/websocket_frame.cpp`
- [ ] `src/rt/websocket_internal.hpp`
- [ ] `src/rt/websocket_socket.cpp`


## Tool receipts

Pending dependency/tool checks and finder runs.

## Site verdicts

Pending source review.

## Ranked defects and counts

Pending source review.
