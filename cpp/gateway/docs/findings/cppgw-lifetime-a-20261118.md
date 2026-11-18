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

- [x] `src/rt/abort.cpp`
- [x] `include/uos/rt/abort.hpp`
- [x] `src/rt/clock.cpp`
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

### `src/rt/abort.cpp`

Abort dispatch is synchronous, so callback reentrancy and ownership during dispatch are the relevant suspension-equivalent boundaries.

- **SAFE** `30, 31, 34, 42, 46, 49, 51, 93, 94, 195, 205, 211, 220` (c/d), Dependent signal registration and pinning. The source temporarily owns interested dependents, the reverse links are weak, and abort moves pins and promoted dependents outside the registry before dispatch.
- **SAFE** `55, 63, 64, 66, 67, 69, 74, 131, 139, 143, 147` (c/d), Listener snapshots and removal traversal. Dispatch owns a snapshot of unique entries plus the executing callback, and live-vector removal exits its traversal immediately.
- **DEFECT (A-ABORT-OWNER)** `62, 69, 74, 81, 95, 228` (c/d), Abort dispatch borrows the controller's state slot. A synchronous listener may release or replace the controller that owns the shared pointer still used by the dispatch epilogue.
- **SAFE** `154, 156, 158, 163, 166, 171, 178` (c), onabort setter and getter in current dispatch paths. The handler slot and listener entry own callback copies and dispatch pins the active callable while assignment or removal changes the slot.
- **SAFE** `187, 189, 190, 191` (c), AbortSignal timeout callback. The unreferenced timer owns the signal by value, the signal has no timer backedge, and firing or loop teardown releases the callback.
- **SAFE** `231, 232, 234, 237, 239, 240, 241` (c), Native cancellation bridge. The completion handler owns the cancellation slot and signal while the listener uses a weak signal link, and completion removes any surviving registration.
- **SAFE** `247, 250, 252, 253, 257, 258, 259, 263, 264` (c), Abortable sleep timer and listener. The timer and listener intentionally retain each other while armed, and abort, timeout, or loop teardown clears an owning callback before releasing the active invocation.

### `include/uos/rt/abort.hpp`

The public header declares the abort bridge and listener handles. The implementation's reentrant dispatch defect is counted at its source.

- **SAFE** `34, 39, 40, 45, 65, 72, 73, 76, 77` (c), Listener identity and invocation. Each identity shares an owning function and the runtime dispatch copies the listener before invoking it, allowing deregistration during the call.
- **SAFE** `59, 62, 88, 91, 94, 102, 105` (b/c), Signal owner and borrowed accessors in current callers. The handle owns its state, callers copy or consume reason and state synchronously, and the separate abort-dispatch borrow is tracked in abort.cpp.
- **SAFE** `115, 125, 126, 127, 128, 129` (a/b/c), Eager abortable wrapper. This ordinary function moves its Task into runInto and returns a Task owning the result state, while its fetch caller gives the native child owned request inputs.

### `src/rt/clock.cpp`

Timer APIs are synchronous. The one Task-returning overload creates an owning result before handing it to the lazy awaiter.

- **SAFE** `13, 14, 21, 22, 78, 81, 85, 87, 91, 93, 99, 104` (c/d), Timer queue ownership and disarm. The queue owns each entry and the timer has a weak queue backedge; handles retain their timer during synchronous dequeue and callback release.
- **UNCLEAR** `129, 134, 135, 176, 181, 185, 186, 187, 191` (c), Self-replacement of injected global hooks. Current production uses defaults and the scoped fixture overrides checked retain their captures. Supporting an override that replaces its own slot while it runs would require proving an independent callable owner.
- **SAFE** `141, 145, 151, 155, 163, 165, 167, 168` (b/c), Timeout handle operations. Ref, unref and refresh are synchronous, lock the weak queue locally, and do not publish a reference to the handle.
- **SAFE** `196, 197, 198, 199, 205, 209, 210, 220, 224, 225` (b/c), Sleep callback and timer construction. The callback and awaiter independently own Promise state, and each newly queued timer owns its moved callback.
- **SAFE** `234, 236, 242, 244, 246, 247, 251, 254, 260, 264, 265, 268` (c/d), Timer dispatch and callback rearm. The dispatcher pins the timer, erases the map entry and moves out the callable before invoking code that may clear or rearm it.
- **SAFE** `272, 273, 276, 277, 280, 281` (c/d), Timer teardown. Teardown owns a detached map and releases moved callbacks after marking the corresponding timers disarmed.

## Ranked defects and counts

Reviewed **3/30 files** and **16 site groups**. A site group can have multiple line anchors and hazard classes under the same verdict.

| Hazard class | DEFECT | SAFE | UNCLEAR |
| --- | ---: | ---: | ---: |
| a. Capturing coroutine closure | 0 | 1 | 0 |
| b. Borrowed coroutine parameter or receiver | 0 | 4 | 0 |
| c. Callback ownership or cycle | 1 | 14 | 1 |
| d. Container invalidation | 1 | 5 | 0 |
| e. GCC 12 await-expression rule | 0 | 0 | 0 |
| **Unique site groups** | 1 | 14 | 1 |

### 1. A-ABORT-OWNER (memory safety in the public runtime API) — Abort dispatch borrows the controller's state slot

`src/rt/abort.cpp:62, 69, 74, 81, 95, 228`

- AbortController::abort passes a reference to its own shared pointer and begins notifying listeners.
- A listener drops the controller or replaces its signal. Dispatch then accesses the ended state or cleans up the replacement's listeners through that borrowed slot.

```diff
--- a/cpp/gateway/src/rt/abort.cpp
+++ b/cpp/gateway/src/rt/abort.cpp
@@ -81,1 +81,1 @@
-    void signalAbort(const std::shared_ptr<State>& signal, Reason reason) {
+    void signalAbort(std::shared_ptr<State> signal, Reason reason) {
```
