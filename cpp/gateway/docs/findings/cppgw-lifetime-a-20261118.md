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
- [x] `include/uos/rt/async.hpp`
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

### `include/uos/rt/async.hpp`

Reviewed the lazy Task adapters, eager Promise combinators, callback return-type branches and generator implementation. The Promise callback defect is reachable through the public API. The production continuations checked consume by-reference inputs synchronously or use owned values.

- **SAFE** `109, 114, 116, 117, 118, 126, 128, 129, 130, 139, 143, 152, 153, 154, 155, 164` (c/d), One-shot jobs and waiting-list declarations. Job moves its erased callable into a local before invocation, and the intrusive list is maintained and unlinked synchronously by the loop implementation.
- **SAFE** `204, 207, 210, 211, 214, 218, 220, 221, 226, 228, 229, 319, 321, 338, 340, 345, 348, 352, 368, 369, 371, 374, 376, 377` (a/b/c), Owned awaiters and public Promise accessors. The wrappers synchronously copy the state or move the task into an owning frame, while value, reason and state accessors borrow from a live Promise in the checked callers.
- **DEFECT (A-PROMISE-GCC-AWAIT)** `219, 375` (e), Implicit shared_ptr conversion in Promise await expressions. Both awaiters form a shared_ptr<PromiseCore> temporary from a typed shared_ptr in the await full expression.
- **SAFE** `233, 235, 240, 245, 248, 249, 273, 274, 277, 279, 280, 285, 579, 580, 582, 583` (b/c), Loop, Clock, Scope and blockOn borrowed API. The synchronous blockOn owns its stop Promise and the current-loop scope is nested inside the named Loop and Clock lifetimes in the checked callers.
- **SAFE** `386, 394, 395, 396, 397, 405, 407, 415, 418, 428, 429, 445, 447, 452, 453, 455, 462, 464, 466` (a/b/c), Synchronous resolver and detached-task entry points. Resolvers share their state, newPromise calls its executor synchronously, and spawn hands the Task to runInto; a Task's own borrowed inputs remain governed by its caller.
- **SAFE** `471, 472, 473, 475, 486, 487, 497, 499, 559, 561, 564, 573, 679, 682, 684, 914, 919, 921, 927` (a/b/c/e), Coroutine holders and finally helpers. callHeld, keepAlive and withFinally keep the callback or explicit owner in an awaited frame; ScopeExit owns its synchronous cleanup and the helpers use named results.
- **DEFECT (A-PROMISE-CALLBACK-INPUT)** `692, 701, 704, 713, 715, 726, 731, 744, 745, 746, 758, 760` (a/b/c), Task-returning Promise callbacks can borrow a destroyed outcome. The Task-keeping branch pins the callable, but a coroutine parameter referencing the fulfilled value or rejection reason still borrows the source state held only by the reaction.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `397, 726, 744, 758, 780, 797, 815, 840, 860, 873, 889, 892` (c), Typed Promise reactions retain pending sources and input tuples. These registrations repeat the self-owning reaction pattern. Apply the source ownership handoff in the ranked fix and collect fulfilled tuple slots without retaining the tuple of input Promises.
- **SAFE** `773, 781, 783, 788, 790, 792, 797, 798, 799` (a/b), finally_ callable and settled source access. The synchronous branch reads the source inside its reaction, and the Task branch retains the callable through completion and the source through the after reaction.
- **SAFE** `519, 520, 521, 522, 529, 530, 537, 541, 542, 545, 549, 810, 820, 823, 824, 835, 841, 849, 869, 878, 890, 898, 902, 909, 910` (a/b/d), Combinator storage and traversal across sibling completion. Registered reactions are synchronous and own their result storage and copied indices, so sibling completion cannot invalidate a borrowed iterator or closure.
- **SAFE** `633, 634, 636, 941, 946, 947, 948, 951, 953, 956, 960, 961, 963, 967, 976, 978, 983, 989, 990, 998, 1001, 1005` (c/d), Generator queue dispatch. A request is moved out before delivery, the launch callable is moved out before execution, and queued work promotes a weak core only for synchronous mutation.
- **SAFE** `663, 664, 666, 671, 672, 675, 677, 1012, 1013, 1016, 1017, 1022, 1026, 1027, 1029, 1035` (b/c), Generator yield and weak waker parking. The Yield task owns its value and holds a weak backedge while parked, and Waker moves the current waiter out before resuming it.
- **SAFE** `1041, 1043, 1046, 1049, 1051, 1058, 1086, 1088, 1091, 1093, 1094, 1095` (a/b/c), Generator body and yielding adapters. The body closure and return slot move into runGenerator's frame, and the public Yield wrappers take a value or copy the weak handle before returning a Task.
- **DEFECT (A-GENERATOR-STEP-OWNER)** `1041, 1058, 1061, 1062, 1063, 1064, 1099, 1101` (b/c), Pending generator step does not keep its generator alive. A returned next or return Task owns only the result, so dropping the last generator handle while the body is executing can strand an outstanding step.
- **SAFE** `1067, 1069, 1073, 1078, 1082, 1114, 1118, 1123, 1124, 1130, 1136` (a/b/d), Delegate and forAwait ownership. The frames own both generator handles or callback and the complete current IteratorResult through the body await and return cleanup.
- **DEFECT (A-GENERATOR-GCC-AWAIT)** `1073, 1123, 1124` (e), Forwarded generator values constructed within await expressions. delegateTo moves a yielded T into a by-value adapter and forAwait may move it into a by-value coroutine callback in the await full expression.

## Ranked defects and counts

Reviewed **6/30 files** and **45 site groups**. A site group can have multiple line anchors and hazard classes under the same verdict.

| Hazard class | DEFECT | SAFE | UNCLEAR |
| --- | ---: | ---: | ---: |
| a. Capturing coroutine closure | 1 | 10 | 0 |
| b. Borrowed coroutine parameter or receiver | 2 | 20 | 1 |
| c. Callback ownership or cycle | 6 | 32 | 2 |
| d. Container invalidation | 2 | 12 | 0 |
| e. GCC 12 await-expression rule | 2 | 2 | 1 |
| **Unique site groups** | 8 | 35 | 2 |

### 1. A-PROMISE-CALLBACK-INPUT (memory safety in the public runtime API) — Task-returning Promise callbacks can borrow a destroyed outcome

`include/uos/rt/async.hpp:692, 701, 704, 713, 715, 726, 731, 744, 745, 746, 758, 760`

- Register a then or catch callback whose Task takes a const reference to the outcome. Drop the source Promise while keeping the derived Promise.
- The queued reaction starts the callback until its first suspension. The reaction finishes and releases the source, then the callback resumes with a dangling outcome reference.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -692,1 +692,2 @@
-    template <class U, class Call> void settleWith(const std::shared_ptr<PromiseState<U>>& target, std::shared_ptr<void> keep, Call&& call) {
+    template <class U, class Call> void settleWith(const std::shared_ptr<PromiseState<U>>& target, std::shared_ptr<void> keep,
+                                                 std::shared_ptr<void> source, Call&& call) {
@@ -701,1 +702,2 @@
-          start(currentLoop(), keepAlive<void>(std::move(keep), runInto(inner, call())));
+          auto owners = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(std::move(keep), std::move(source));
+          start(currentLoop(), keepAlive<void>(std::move(owners), runInto(inner, call())));
@@ -731,1 +733,1 @@
-      detail::settleWith(target, callback, [&] { return detail::invokeWithValue<T>(*callback, *source); });
+      detail::settleWith(target, callback, source, [&] { return detail::invokeWithValue<T>(*callback, *source); });
@@ -745,2 +747,2 @@
-      if (source->status == detail::PromiseCore::Status::rejected) detail::settleWith(target, rejected, [&] { return (*rejected)(source->error); });
-      else detail::settleWith(target, fulfilled, [&] { return detail::invokeWithValue<T>(*fulfilled, *source); });
+      if (source->status == detail::PromiseCore::Status::rejected) detail::settleWith(target, rejected, source, [&] { return (*rejected)(source->error); });
+      else detail::settleWith(target, fulfilled, source, [&] { return detail::invokeWithValue<T>(*fulfilled, *source); });
@@ -760,1 +762,1 @@
-        detail::settleWith(target, callback, [&] { return (*callback)(source->error); });
+        detail::settleWith(target, callback, source, [&] { return (*callback)(source->error); });
```

### 2. A-ABORT-OWNER (memory safety in the public runtime API) — Abort dispatch borrows the controller's state slot

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

### 3. A-PROMISE-REACTION-CYCLE (retention on long-running loops) — Pending Promise reactions self-own their source

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

### 4. A-GENERATOR-STEP-OWNER (public runtime liveness) — Pending generator step does not keep its generator alive

`include/uos/rt/async.hpp:1041, 1058, 1061, 1062, 1063, 1064, 1099, 1101`

- Call next on a generator whose body first yields through a microtask. Keep the step Task and release the generator handle before that microtask runs.
- yieldTo finds the weak core expired and abandons the body. The outstanding step has no owner capable of fulfilling or rejecting it.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -1061,5 +1061,6 @@
-    template <class T> Task<IteratorResult<T>> requestStep(const std::shared_ptr<GeneratorCore<T>>& core, bool isReturn) {
+    template <class T> Task<IteratorResult<T>> requestStep(std::shared_ptr<GeneratorCore<T>> core, bool isReturn) {
       auto result = std::make_shared<PromiseState<IteratorResult<T>>>();
       core->enqueue(isReturn, result);
-      return awaitOwned(std::move(result));
+      auto waiting = awaitOwned(std::move(result));
+      return keepAlive<IteratorResult<T>>(std::move(core), std::move(waiting));
     }
```

### 5. A-LOOP-TEARDOWN-OWNER (wrong-loop teardown and retention) — Scoped Loop teardown runs callbacks under the ambient loop

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

### 6. A-PROMISE-GCC-AWAIT (GCC 12 portability rule) — Implicit shared_ptr conversion in Promise await expressions

`include/uos/rt/async.hpp:219, 375`

- awaitOwned or awaitState calls awaitSettled directly with a typed state. The call converts it to an owning base shared_ptr inside co_await.
- The retained GCC 12 coroutine rule rejects this expression shape because await temporaries can be destroyed twice. This occurrence has static evidence.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -219,1 +219,2 @@
-      co_await awaitSettled(state);
+      auto ready = awaitSettled(state);
+      co_await std::move(ready);
@@ -375,1 +376,2 @@
-      co_await detail::awaitSettled(state);
+      auto ready = detail::awaitSettled(state);
+      co_await std::move(ready);
```

### 7. A-GENERATOR-GCC-AWAIT (GCC 12 portability rule) — Forwarded generator values constructed within await expressions

`include/uos/rt/async.hpp:1073, 1123, 1124`

- Instantiate the forwarding helpers with an owning yielded value, such as bytes, and a by-value callback. The moved parameter is materialized in the await expression.
- The retained GCC 12 coroutine rules require creating that operation in a separate statement. This occurrence has static evidence.

```diff
--- a/cpp/gateway/include/uos/rt/async.hpp
+++ b/cpp/gateway/include/uos/rt/async.hpp
@@ -1073,1 +1073,2 @@
-          co_await outer(std::move(*step.value));
+          auto passing = outer(std::move(*step.value));
+          co_await std::move(passing);
@@ -1123,2 +1124,3 @@
-        if constexpr (std::is_same_v<Result, Task<bool>>) proceed = co_await body(std::move(*step.value));
-        else co_await body(std::move(*step.value));
+        auto handling = body(std::move(*step.value));
+        if constexpr (std::is_same_v<Result, Task<bool>>) proceed = co_await std::move(handling);
+        else co_await std::move(handling);
```
