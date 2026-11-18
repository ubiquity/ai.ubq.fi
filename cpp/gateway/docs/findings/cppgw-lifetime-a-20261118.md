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
- [x] `include/uos/rt/clock.hpp`
- [x] `src/rt/async.cpp`
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

### `include/uos/rt/clock.hpp`

The injected Clock methods are synchronous. Loop's borrowed Clock dependency is assessed in async.cpp and async.hpp.

- **SAFE** `75, 78, 94, 96, 99` (b/c), Timeout timer ownership. Timeout is a shared handle and the implementation consumes its state accessor within synchronous timer operations.
- **SAFE** `102, 108, 111, 115, 117, 127, 128, 129, 130, 134, 141, 145, 151, 153` (b/c), Timer registration and scheduling API. Registration accepts an owning callable by value, clearing borrows only synchronously, and sleep creates and owns its result independently of the caller.

### `src/rt/async.cpp`

Read the complete loop implementation and the PromiseCore callbacks. The process loop is intentionally permanent. Scoped loops have an explicit teardown path.

- **SAFE** `147, 148, 149, 156, 163, 379, 380, 385` (b/c), Loop owns the context and timer queue and borrows its Clock. A scoped Loop's clock is declared before it in the runtime fixture, and the permanent default Loop uses the static SystemClock.
- **SAFE** `178, 184, 186, 190, 191, 193, 194, 198, 199, 203, 209, 383, 384` (b/c), Asio executor's Loop implementation pointer. The Loop contract confines I/O and executor users to the owner lifetime, and execute owns dispatched handlers or discards them once the context is gone.
- **SAFE** `215, 216, 284, 288, 289, 290, 291, 293, 298, 299` (c/d), Microtask callback dispatch. Each one-shot job is moved out before it can reenter the loop, and rejection candidates live in a detached vector while they are inspected.
- **SAFE** `219, 227, 238, 242, 243, 244, 245, 246, 408, 413, 422` (c/d), Intrusive Promise waiting-list mutation. Promise destruction unlinks synchronously, and teardown unlinks and pins each node before destroying its reactions and any continuations they own.
- **DEFECT (A-LOOP-TEARDOWN-OWNER)** `252, 257, 258, 261, 263, 266, 268, 367, 368, 396, 397, 400, 401, 443, 445` (c), Scoped Loop teardown runs callbacks under the ambient loop. A destroyed frame can queue a microtask from a ScopeExit while the dying loop is absent from currentLoop, letting that job survive in an unrelated loop.
- **SAFE** `313, 316, 319, 322, 325, 332, 338, 349, 355, 364, 367, 372, 376, 454` (b/c), Run-loop stop predicate and scope guards. The synchronous caller holds the stop Promise through runLoop, and the local guards restore the running flag and scope before returning.
- **SAFE** `410, 414, 416, 420, 424, 425, 427, 431` (c/d), Promise settlement traversal. Reactions move into the microtask queue without invocation during traversal, and unhandled rejections keep a separate shared owner until the checkpoint.
- **SAFE** `437, 438, 439, 443, 481, 482` (a/b/c), Native start and user microtask registration. Asio owns the moved task and a captureless completion handler, and the queue owns the moved user callable.
- **UNCLEAR** `449, 450, 451` (b/c/e), Parked awaiter and pending Promise ownership. The waiter job owns the coroutine that owns the Promise; settlement and loop teardown break the cycle. Identifying indefinitely abandoned request waiters would establish where this topology retains request state on the permanent loop.
- **SAFE** `456, 457, 458, 462, 465, 467, 470, 472, 476, 477, 478` (a/b/c/e), Waker, abandon and microtask initiations. Each initiation is named and synchronous, and the handler goes to a weakly linked slot, to the loop, or is intentionally discarded.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `485, 490, 491, 492, 494, 497` (c/d), Pending Promise reactions self-own their source. A dropped never-settled input keeps its reaction and combinator output alive until Loop destruction, including losers of a completed combination on the permanent loop.

## Ranked defects and counts

Reviewed **5/30 files** and **29 site groups**. A site group can have multiple line anchors and hazard classes under the same verdict.

| Hazard class | DEFECT | SAFE | UNCLEAR |
| --- | ---: | ---: | ---: |
| a. Capturing coroutine closure | 0 | 3 | 0 |
| b. Borrowed coroutine parameter or receiver | 0 | 11 | 1 |
| c. Callback ownership or cycle | 3 | 24 | 2 |
| d. Container invalidation | 2 | 8 | 0 |
| e. GCC 12 await-expression rule | 0 | 1 | 1 |
| **Unique site groups** | 3 | 24 | 2 |

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

### 2. A-PROMISE-REACTION-CYCLE (retention on long-running loops) — Pending Promise reactions self-own their source

`src/rt/async.cpp:485, 490, 491, 492, 494, 497`

- all registers a reaction whose owning source capture points to the state storing that reaction. Another input can reject the combined Promise.
- Drop the pending input and every resolver. Its last ownership is the reaction inside itself, so neither that source nor the captured output/continuations can be reclaimed.

```diff
--- a/cpp/gateway/src/rt/async.cpp
+++ b/cpp/gateway/src/rt/async.cpp
@@ -416,1 +416,3 @@
-      currentLoop().impl().microtasks.push_back(std::move(reaction));
+      currentLoop().impl().microtasks.push_back(
+          [source = shared_from_this(), job = std::move(reaction)]() mutable { job(); });
+
@@ -424,1 +426,4 @@
-    for (auto& reaction : reactions) impl.microtasks.push_back(std::move(reaction));
+    for (auto& reaction : reactions) {
+      impl.microtasks.push_back(
+          [source = shared_from_this(), job = std::move(reaction)]() mutable { job(); });
+    }
@@ -492,1 +497,1 @@
-      source->react([source, out, remaining] {
+      source->react([source = source.get(), out, remaining] {
```

### 3. A-LOOP-TEARDOWN-OWNER (wrong-loop teardown and retention) — Scoped Loop teardown runs callbacks under the ambient loop

`src/rt/async.cpp:252, 257, 258, 261, 263, 266, 268, 367, 368, 396, 397, 400, 401, 443, 445`

- A pending coroutine belongs to a local Loop. Loop::run or blockOn returns and its LoopScope restores the previous loop.
- Destroying the local Loop drops the frame. A destructor that queues a cleanup job sends it to the previous or permanent process loop, outside the teardown drain.

```diff
--- a/cpp/gateway/src/rt/async.cpp
+++ b/cpp/gateway/src/rt/async.cpp
@@ -252,1 +252,2 @@
   Loop::Impl::~Impl() {
+    LoopScope scope(owner);
```
