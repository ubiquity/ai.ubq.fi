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
- [x] `src/rt/process_loop.cpp`
- [x] `include/uos/rt/process.hpp`
- [x] `src/rt/stream.cpp`
- [x] `include/uos/rt/stream.hpp`
- [x] `src/rt/http.cpp`
- [x] `include/uos/rt/http.hpp`
- [x] `include/uos/rt/fetch.hpp`
- [x] `include/uos/rt/fetch_transport.hpp`
- [x] `include/uos/rt/gzip.hpp`
- [x] `include/uos/rt/websocket.hpp`
- [x] `src/rt/fetch.cpp`
- [x] `src/rt/fetch_transport.cpp`
- [x] `src/rt/fetch_transport_codec.cpp`
- [x] `src/rt/fetch_transport_http.cpp`
- [x] `src/rt/fetch_transport_internal.hpp`
- [x] `src/rt/fetch_transport_net.cpp`
- [x] `src/rt/gzip.cpp`
- [x] `src/rt/tls.cpp`
- [x] `src/rt/tls_internal.hpp`
- [x] `src/rt/websocket.cpp`
- [x] `src/rt/websocket_client.cpp`
- [x] `src/rt/websocket_frame.cpp`
- [x] `src/rt/websocket_internal.hpp`
- [x] `src/rt/websocket_socket.cpp`

## Tool receipts

The restored runner is Ubuntu 24.04.5 LTS on Linux ARM64. Clang and clang-tidy
are 21.1.8. The available dependencies are Boost 1.92.0 static JSON and Container,
SQLite 3.54.0, OpenSSL 3.0.13, zlib 1.3, ICU 74.2, Brotli 1.1.0 and Zstd 1.5.5.
The focused checks below do not qualify the Unicode differential suite with ICU 74.2.

The retained clang-tidy run completed all **17/17** in-area source translation units
with five selected checks, zero warnings, zero frontend failures and zero timeouts.
The run took 38.321 seconds with two workers and peak child RSS of 471,416 KiB.
The coroutine control emitted exactly three expected diagnostics in 0.662 seconds.
Headers were analyzed through these translation units and reviewed directly.

The retained focused sanitizer logs include two heap-use-after-free reports and one
leak report. The repo fixture boundary used fresh network, mount and PID namespaces
through passwordless sudo after unprivileged namespace setup failed. The probe
verified loopback, an unreachable outside route, private `/tmp` and uid 1001.
Detailed run receipts and remaining verification are being reconciled.

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

### `src/rt/process_loop.cpp`

Reviewed both pidfd and SIGCHLD branches, subprocess pipe streams, process listeners and BunFile stream adapters. The production server registers process signals on the process loop.

- **SAFE** `45, 51, 53, 73, 76, 79, 84, 87` (b/c/e), POSIX child exit wait and signal names. The watcher owns its signal_set and PID throughout the wait, the error token is named, and Darwin's string views point into static storage.
- **DEFECT (A-PROCESS-SIGNAL-CONTEXT)** `100, 102, 107, 108, 119, 124, 125, 134, 136, 142, 148` (b/c), Static process signal registry retains a scoped Asio context. A listener registered on a scoped Loop can leave a static signal_set whose service and executor have already died.
- **SAFE** `123, 124, 126, 129, 130, 150, 154, 156, 158, 159, 161, 165, 168, 174, 263, 264, 266, 271, 272, 274, 279, 281` (c/d), Signal wait generation and synchronous listener dispatch. The wait callback captures a generation by value; listener delivery snapshots owning identities and erases once entries before invoking any callback.
- **SAFE** `231, 232, 234, 235, 236, 239, 240, 244, 248, 251, 252, 254, 255, 257, 259` (a/b/c/e), Subprocess pipe read and cancellation. The source closures own the descriptor, the forwarding pull moves an owned controller into the child frame, and the frame owns its I/O buffer and token through completion.
- **SAFE** `287, 288, 295, 296, 299, 300, 305, 306, 307, 308, 310, 311, 312, 313, 318, 319, 320, 323, 327, 328` (b/d/e), FileSink input views and suspended flush. write synchronously copies incoming views, and each flush owns the stable descriptor via its state and the moved chunk independently of later changes to pending.
- **SAFE** `330, 333, 344, 345, 346, 362, 363, 366, 367, 369, 370, 385, 386, 391, 395, 400, 405, 410, 415, 417, 430` (b/c/e), Child-exit observers and public subprocess accessors. The watchers own state and native descriptors across waits, while accessors synchronously copy owning handles and kill consumes its view before returning.
- **SAFE** `433, 437, 446, 448, 451, 459, 465, 474, 513, 524, 527, 528, 531, 533, 534, 536, 538, 540, 543, 545` (a/b/c/d), Spawn setup and watcher detachment. The borrowing lambdas and argv or env pointers are synchronous, and the detached watchers receive state and their native wait inputs by value.
- **SAFE** `551, 555, 560, 561, 570, 581, 591, 600, 602, 604, 605, 606, 608` (b/c), BunFile stream callbacks and descriptor lifetime. The source owns its pathname and descriptor, the synchronous pull and cancel closures own that source, and close clears the fd before teardown.

### `include/uos/rt/process.hpp`

The ProcessLoop public interface is split from its implementation in process_loop.cpp and the loop-free functions in process.cpp.

- **SAFE** `80, 84, 90, 92, 93, 95, 100, 103, 106, 110, 111, 112, 113, 117, 118` (b/c), Signal Listener closure and signal-name views. Registration copies the owning Listener identity, delivery invokes an owned snapshot, and the event/name views are used synchronously or point to static signal-name strings.
- **SAFE** `137, 141, 143, 144, 146, 148, 151, 155, 158, 162, 164, 165, 168, 178, 181, 190` (b/c), FileSink and Subprocess handle APIs. Writes and kill consume their views synchronously, and deferred work gets shared state from the handle or returns an owning stream or Promise copy.
- **SAFE** `212, 214, 216, 220, 223, 225` (b/c), BunFile path and stream declaration. The object owns its path and stream copies that path into the source before returning, allowing the BunFile wrapper to be a temporary.

### `src/rt/stream.cpp`

This translation unit contains synchronous reason/value construction and explicit ByteChunk template instantiations. The instantiated suspension sites are recorded in stream.hpp.

- **SAFE** `7, 10, 11, 13, 16, 19` (b), Tee cancellation reason construction. The helpers consume the borrowed exception handles synchronously and the resulting exception owns both reasons and its JSON representation.

### `include/uos/rt/stream.hpp`

Reviewed eager reader registration, source callback type erasure, reentrant cancellation, branch teeing and generator adapters. A copied controller is a strong stream owner. A copy kept in closure-owned state therefore needs an independent release path.

- **SAFE** `38, 46, 48, 49, 55, 56, 58, 60, 72, 73, 74, 136, 251, 294, 295, 296, 297, 373, 378` (a/b/c), Source algorithm callable lifetime. The start source is owned by the constructor, runtime pull/cancel calls use local algorithm copies and runAsync retains the erased coroutine closure throughout each asynchronous call.
- **UNCLEAR** `49, 72, 74` (b), Borrowing callback parameters through the by-value erased signature. The current callbacks checked take owned values or complete synchronously. A supported ABI check with a Task callback taking const A& would establish whether the materialized std::function parameter survives throughout its await.
- **DEFECT (A-STREAM-GCC-AWAIT)** `72, 540` (e), Stream callback and yield arguments materialized inside awaits. runAsync forwards an owning erased argument and iterateStream moves an owning chunk within the await full expressions.
- **SAFE** `103, 104, 111, 124, 128, 135, 136, 137, 140, 146, 305, 307, 311, 316, 321, 324, 376, 377` (c), Controller and active pull ownership. The executing controller and pull reaction pin state during use; a callback-owned controller cache can create a separate persistent backedge, assessed at each such source.
- **DEFECT (A-PROMISE-REACTION-CYCLE)** `137, 138, 252, 254, 378, 380, 570, 571` (c), Stream reactions capture their own pending Promise states. The pull, cancel, start and tee closed callbacks repeat the pending-source cycle from PromiseCore. Their source states need ownership only during delivery.
- **SAFE** `151, 153, 154, 155, 157, 162, 165, 171, 173, 174, 182, 183, 186, 190, 191, 269, 270, 283, 284, 286` (c/d), Stream and reader queue drain boundaries. Chunk and reader-request entries are moved out before settlement, reader release pins the stream, and Promise settlement queues reactions outside these synchronous traversals.
- **SAFE** `203, 204, 215, 216, 218, 221, 223, 225, 226, 231, 233, 234, 334, 346, 348, 359, 361, 408, 419, 425, 426, 428, 433` (b/c/d), Read registration and reader accessors. Read registration happens synchronously and returns an owned result state, and no borrow of the reader's stream is used after a pull callback can reenter.
- **DEFECT (A-STREAM-CANCEL-OWNER)** `238, 249, 251, 252, 253, 262, 264, 336, 338, 352, 355, 396, 398, 401, 404` (c/d), Stream cancel borrows the owner slot through callback invocation. The callback can release the reader or replace the stream handle while the cancellation epilogue still dereferences its shared_ptr slot.
- **DEFECT (A-STREAM-TEE-READ-CYCLE)** `444, 445, 446, 453, 454, 458, 460, 464, 469, 475, 479, 485, 486, 493, 496, 502, 554, 557, 558, 559, 561, 562, 567, 568` (b/c), In-flight tee read keeps abandoned branches' source alive. Weak branch references avoid one direct backedge, while an unfulfilled source read still owns the teeRead continuation that owns the source through TeeState.
- **SAFE** `505, 507, 510, 513, 514, 515, 517` (b/c), Tee cancellation releases its tee owner before waiting. The cancel coroutine transfers to the shared completion state once branch bookkeeping is done, and adoption owns the source cancellation operation.
- **DEFECT (A-PROMISE-GCC-AWAIT)** `516` (e), Tee cancellation converts the shared Promise state inside the await. The typed shared_ptr is converted to a base shared_ptr in the co_await full expression. Use the same named Task as for awaitOwned.
- **SAFE** `461, 472, 473, 488, 489, 491, 520, 522, 525, 529, 531, 535, 540, 545, 546, 547, 581, 582, 583` (a/b/d), Tee delivery and stream iterator step storage. The current step belongs to the coroutine, promoted branch handles are local, and the iteration frame owns its reader while it awaits the stored read or cancel.

### `src/rt/http.cpp`

Read the body readers and stream adapters, clone/transfer paths, synchronous FormData parsing and static MIME data. This file models Fetch values. The server transport lives outside Area A.

- **SAFE** `101, 106, 108, 109, 110, 113, 114, 115, 989, 990, 991, 1006, 1008, 1010, 1012` (b/c), Blob data and temporary Blob handles. The ordinary member wrappers copy the shared immutable bytes into the coroutine or stream before the receiver can die.
- **SAFE** `145, 146, 148, 150, 155, 156, 158, 160, 747, 752, 754, 757` (c/d), Buffered body streams and their source callbacks. The source owns bytes and offset without a stream backedge, and the copied pull algorithm owns both for synchronous iteration and close.
- **SAFE** `203, 205, 217, 555, 558, 578, 585, 601, 607, 617, 652, 661, 670, 683, 685, 1034, 1054, 1059, 1065` (b/d), FormData parse, serialize and entry mutation views. All parser views and entry iterators are consumed synchronously under the owned body or FormData and the products own their bytes and strings.
- **SAFE** `696, 698, 699, 700, 704, 708, 713, 718, 724, 730, 733, 894, 912, 913, 918, 925, 931, 1097, 1147, 1157, 1166, 1173, 1188, 1195, 1204, 1217, 1240, 1247, 1256` (b/c), Body extraction, clone, header and transfer wrappers. The reference captures are invoked immediately by visit and body cloning transfers owning stream handles synchronously. Tee retention is tracked at its implementation.
- **SAFE** `777, 789, 790, 797, 803, 806, 809, 810, 812, 813, 815, 817, 833, 834, 835, 839, 841, 847, 849, 857, 859, 862, 865, 867, 869, 872, 878, 879, 883, 1141, 1145, 1234, 1238` (b/c/d/e), Request and Response read tasks and their inputs. The wrappers snapshot owned data or a reader before returning, and each frame owns the read operation, complete step, accumulated bytes and form boundary during later awaits.
- **UNCLEAR** `771, 797, 803, 809, 812, 813, 817, 1141, 1234` (b/c), Abandoned streamed-body reader lock. The eager wrapper locks the body before the lazy Task starts and release is reached on success. Decide the required lock and cancellation behavior when a caller discards an unstarted Task or a stream read rejects, then exercise that behavior with an independently retained body handle.

### `include/uos/rt/http.hpp`

Reviewed the Blob, File, FormData, Request and Response API. The public Task methods are ordinary entry points that acquire ownership before the corresponding implementation coroutine starts.

- **SAFE** `48, 51, 52, 62, 66, 68, 69, 70, 72, 75, 76, 80, 81, 91, 95, 101` (b), Blob and File construction, getters and asynchronous reads. Value construction consumes views synchronously, borrowed getters refer to the owning handle, and asynchronous reads copy the backing shared_ptr at call time.
- **SAFE** `118, 120, 121, 122, 124, 125, 126, 129, 131, 132, 135, 136, 137, 138, 139, 142, 143` (b/d), FormData borrowed entries and iterators. Mutators and accessors execute synchronously. The reader and serializer paths finish entry traversal before returning or suspending.
- **SAFE** `177, 180, 184, 189, 192, 193, 201, 205, 209, 210, 211, 216, 219, 224, 229, 230, 231, 235, 236` (b/c), Response handle and body methods. Response owns shared state, reference getters are synchronous in the checked callers, and the body adapters return tasks with independently owned inputs.
- **SAFE** `249, 261, 262, 263, 265, 268, 269, 279, 287, 288, 290, 292, 293, 294, 296, 298, 299, 300, 301, 302, 303, 307, 308, 312, 313, 314, 323` (b/c), Request construction, body APIs and transfer. The constructors consume URL and init borrows synchronously, property access borrows a live handle, and each deferred body reader takes owned state from the ordinary wrapper.

### `include/uos/rt/fetch.hpp`

The statically erased Fetch and MockTransport callback signatures are the ownership boundary. Current callers mostly use by-value callback inputs. The generic Fetch constructor also accepts reference-taking coroutine callables.

- **SAFE** `27, 29, 38, 39, 40, 41, 43, 44, 45, 48, 49, 50, 54, 56, 58, 64, 65, 70, 89, 90, 100` (a/b/c), Ordinary Fetch wrapping and scoped global overrides. The synchronous Response wrapper captures its callable by shared owner and returns a value-owning Task, and an override owns its copied predecessor.
- **DEFECT (A-FETCH-CALLBACK-INPUT)** `20, 27, 28, 29, 33, 38, 40, 41, 49` (a/b), Fetch erasure can discard input storage before a borrowing coroutine starts. The invocability constraint accepts reference-taking callables, but the by-value erased invocation can destroy its RequestInfo and RequestInit before their borrowed uses.
- **SAFE** `81, 83, 84, 102, 105, 106, 107, 109, 112, 113` (b/d), Transport input and recorded request views. The virtual request parameter owns a handle and the recorded vector view is consumed while the MockTransport is held. The lifetime of the virtual receiver and callback is assessed at their implementations.

### `include/uos/rt/fetch_transport.hpp`

Reviewed the options and transport interface. The implementation copies the shared client state at the public send boundary, and the scoped-loop disposal path is covered at Pool.

- **SAFE** `28, 31, 34, 36, 40, 43, 46, 58, 59, 60, 64, 66, 68, 71, 76, 77` (b/c), Client options, send wrapper and installation. Options and requests cross entry points by value, deferred send takes a shared ClientState, and synchronous pool access and global installation use owning transport handles.

### `include/uos/rt/gzip.hpp`

The incremental codec interface exposes synchronous borrows with explicit validity windows. The coroutine wrappers take Bytes by value.

- **SAFE** `100, 101, 102, 103, 106, 108, 109, 110, 111, 113, 115, 121` (b/d), Inflater input and output spans in gateway callers. The fetch decoder drains and copies each inflater piece synchronously while the calling buffer and unique codec state remain alive.
- **SAFE** `129, 135, 138, 146, 149, 153` (a/b/c), One-shot, callback and coroutine codec APIs. The piece callback runs synchronously before the borrowed span changes and both Task APIs own their bytes across the microtask.

### `include/uos/rt/websocket.hpp`

Reviewed the listener and callback shapes, client and server ownership contracts and the upgrade handoff. Event views and target pointers are call-scoped in the checked production handlers.

- **SAFE** `57, 63, 69, 87, 93, 94, 95, 171, 173, 175, 179, 181, 187, 189, 194, 352, 359, 362` (b/d), Incoming values and event borrows in the checked callers. The native dispatch owns the active event, gateway listeners consume its raw pointers synchronously, and scheduled work copies payload or response data first.
- **SAFE** `105, 107, 108, 110, 111, 113, 115, 118, 132, 135, 138, 139, 141, 142, 144, 149, 150, 153, 160, 162, 166` (c/d), Listener identity and declared EventEmitter storage. The emitter owns the registered functions and dispatch uses owning snapshot entries. This pins callable identity through reentrant listener removal.
- **SAFE** `239, 243, 256, 259, 264, 265, 285, 286, 287, 288, 296, 302, 305, 311, 317, 318, 319, 320, 321, 322, 323, 324, 337, 347` (b/c), Client construction, send callbacks and event attributes. Asynchronous sends transfer owning payload and callable values, and attribute wrappers belong to the socket's registry. Receiver ownership during delivery is assessed in the implementation.
- **UNCLEAR** `326, 330, 331, 332, 333` (c), Self-replacement of injected buffered-amount getter. The production getter is fixed. Supporting a configured getter that overwrites its own callable requires testing that invocation with an independently pinned callable.
- **SAFE** `368, 372, 399, 403, 407, 409, 414, 415, 427, 429, 435, 437, 439, 442, 444, 447` (b/c/d), Upgrade API borrows and cross-loop ownership. Protocol and upgrade callbacks run synchronously against the caller's request, the socket is moved into native state, and client list access returns an owning vector snapshot.

### `src/rt/fetch.cpp`

Reviewed both request-init paths, both abort windows, the transport seam and the replaceable global Fetch slot. The installed HTTP client has an owning send wrapper. The polymorphic transport API also permits coroutine member implementations.

- **SAFE** `23, 28, 35, 43, 44, 49, 50, 55, 59, 61, 64, 65, 155, 160, 161, 164, 165, 168, 170, 187, 199, 200` (b/d), Request preparation and forwarding into owned tasks. The validation wrappers are synchronous, variant references end before dispatch, and rejected or sending tasks take owned reasons or Request handles.
- **SAFE** `77, 80, 84, 87, 88, 90, 91, 94, 95, 97, 100, 103, 104, 105, 106, 113, 115, 116, 118, 119, 121, 125, 126, 140, 145, 146` (a/b/c/d), Response abort and cancel delivery while active. The callback promotes a weak body for the whole invocation, pull and cancel frames own their parameters, and detached reader cancellation has an owned result independent of its caller.
- **DEFECT (A-FETCH-ABORT-BODY-CYCLE)** `70, 72, 73, 74, 77, 112, 114, 115, 120, 123, 124, 125, 126, 127, 128` (c), Dropped abortable response body is kept by its own controller. The wrapper's algorithms own AbortableBody and AbortableBody owns the controller of that same stream. Normal completion clears the algorithms but an unread HWM-zero response may be abandoned without a terminal callback.
- **DEFECT (A-FETCH-ABORT-TRANSPORT)** `131, 132, 134, 135, 136, 137, 138, 195` (b/c), Abort can outlive the owning virtual transport frame. The default HTTP client copies ClientState before suspension. A transport whose member coroutine uses its receiver can survive the front-end await without owning the virtual object.
- **SAFE** `134, 139, 140, 141, 142, 143, 144, 146, 149, 152` (b/c/e), Abort-window result and owned error callback. The send frame owns the signal and response and schedules a source with its own Reason. The wrapper uses synchronous factories and the awaited abortable Task is named.
- **DEFECT (A-FETCH-REENTRANT-FUNCTION)** `176, 177, 178, 181, 183, 189, 191, 193` (a/b/c), Fetch calls a replaceable slot before reliably taking ownership. Argument evaluation may invoke the old callable before copying its owner. A synchronous injected function can replace the active slot and drop its own closure during that invocation.
- **DEFECT (A-FETCH-CALLBACK-INPUT)** `176, 178, 181, 183` (a/b), Injected async Fetch arguments cross the erasure lifetime boundary. The wrapper retains the callable but the by-value std::function invocation owns its arguments only within that call expression.
- **DEFECT (A-MOCK-HANDLER-OWNER)** `204, 205, 206` (a/b/c), Mock transport returns a callback task without its handler owner. The ordinary send method records a copy but passes a separate erased Request to its handler and returns the child task directly.

### `src/rt/fetch_transport.cpp`

Reviewed DNS, both connection branches, concurrent upload/download, stale retry, redirects, EOF/error/cancel and pooling handoff. The writer starts independently so an early response can be delivered before upload completion.

- **SAFE** `55, 57, 58, 60, 61, 63, 66, 69, 81, 93, 94, 95, 96, 103, 104, 105, 106, 109, 110, 111` (b/d/e), Owned request body and DNS inputs. The request frame owns both handles and copies bytes. The resolver frame owns host, service, resolver, error token and the final endpoint copy.
- **SAFE** `95, 97, 98, 99, 100, 106, 107` (b/c), DNS cancellation closure. The cancellation slot retains only a weak resolver reference, and the active resolution keeps the resolver and slot context alive until completion or teardown.
- **SAFE** `117, 118, 124, 125, 130, 131, 133, 134, 139, 143, 144, 146, 147, 155, 156, 157, 161, 162` (b/d/e), TCP connection, TLS receiver and pool acquisition. The private endpoint vector cannot be mutated by other work, and the connecting frame owns the client, TLS context owner and Connection across the handshake.
- **SAFE** `182, 184, 186, 188, 189, 193, 194, 197, 198, 203, 204, 209, 210, 219, 222, 223, 225, 226, 229, 230, 231, 270, 274, 275, 279` (a/b/c/d), Detached write task, dynamic buffers and local cleanup. The writer owns connection, reader, upload inputs and each write owner. The synchronous scope-exit reference is to a local declared before its guard.
- **DEFECT (A-FETCH-UPLOAD-STRANDED)** `173, 182, 203, 204, 209, 210, 215, 216, 274, 276, 277, 279, 296, 298, 299, 349, 350, 368, 372, 378, 475, 478, 479, 530, 537, 555` (b/c), Terminal connection close can strand a detached upload reader. Closing the socket wakes its native operations. A writeRequest frame waiting on a JavaScript-style reader Promise also needs its reader released or cancelled.
- **SAFE** `264, 265, 269, 271, 280, 282, 284, 288, 293, 294, 302, 306, 307, 308, 314, 316, 318, 319, 320, 323, 328` (b/d), Exchange head parse, retry and connection handoff. Response head fields are owned and the input view ends before fill; retry passes owned configuration and connection into the exchange frame.
- **SAFE** `334, 335, 336, 341, 349, 354, 355, 357, 360, 368, 370, 373, 383, 384, 389, 390, 396, 397, 402, 409, 410, 464, 466, 467, 470, 471, 474, 475, 476, 478, 479, 483, 488` (b/c/d), Response pump, parser views and source closures. The stream owns the pump while active, pool backedges are weak, and parser views are consumed before queue mutation or suspension. Scope-bound loop ownership is required by docs/rt/async.md.
- **SAFE** `413, 416, 417, 419, 421, 422, 426, 427, 428, 429, 436, 437, 438, 440, 445, 446, 447, 455, 457, 458` (a/b/c/d/e), Native fill receiver during response cancellation. The named fill Task pins the Connection through the native read. Cancellation sets the shared flag before dropping the pump's slot, and the continuation checks it before using that slot.
- **DEFECT (A-FETCH-RESPONSE-STRANDED)** `135, 214, 215, 216, 236, 237, 238, 416, 436, 437, 438, 440, 441, 452, 455, 474` (b/c), Post-head upload failure leaves a response read parked. The detached upload writer can close the Connection with a body failure after the response exists. The response pull treats the resulting aborted read as a silent successful pull.
- **SAFE** `492, 493, 496, 497, 498, 501, 502, 503, 504, 513, 514, 515, 518, 519, 520, 521, 522, 523, 526, 527, 533, 535, 549, 552, 553, 559, 561, 562, 563, 567, 568, 581, 584, 589, 595, 596` (b/d/e), Redirect drain, header edits and public send owner. The redirect frame owns request metadata, next URL and exchange. Header iteration is synchronous, and the public ordinary wrapper copies ClientState into the deferred operation.

### `src/rt/fetch_transport_codec.cpp`

Reviewed native and dynamically resolved codec branches. Decoder operations and function-table initialization run synchronously.

- **SAFE** `24, 25, 27, 34, 35, 37, 40, 44, 45, 47, 52, 64, 67, 73, 76, 78, 79, 80, 85, 88` (b/d), Zlib adapter and borrowed decoder buffers. The response pump owns the decoder and input during the synchronous write, and each inflater output span is copied before the next read. On failure the pump stops using the decoder.
- **SAFE** `122, 124, 127, 129, 131, 134, 135, 137, 140, 147, 150, 154, 157, 160, 163, 164, 174, 179, 181` (b/c), Brotli function table and streaming buffer ownership. Each decoder owns its C state and borrows a static API table, backed by the process-lifetime loader handle in the fallback. The C call uses stack buffers whose bytes are appended before returning.
- **SAFE** `222, 224, 227, 229, 231, 234, 235, 238, 241, 248, 250, 252, 256, 259, 261, 262, 263, 265, 271, 276, 278` (b/c), Zstd function table and streaming buffer ownership. Each decoder frees its C stream through the static API table; its input view and output buffer descriptors are used entirely in one synchronous write.
- **SAFE** `285, 287, 288, 300, 301, 302, 303, 304, 309, 311, 314, 315, 316, 320, 321, 323, 324, 327, 329, 332, 334, 339, 340, 345, 346` (b/d), Decoder selection and coding-name views. The code copies each view into its local vector before taking the selected reference, performs no later mutation of that vector and passes the URL into an owning string.

### `src/rt/fetch_transport_http.cpp`

Reviewed the complete HTTP wire helpers, including incomplete and malformed parser returns. These helpers operate synchronously on caller-owned buffers.

- **SAFE** `18, 26, 32, 37, 44, 47, 71, 90, 102, 107, 121, 122, 125, 144, 148, 152, 154, 155, 158` (b), Header token views and error construction. Trim, token and line-parser views are consumed within the active caller, and FetchError takes value copies of the code and URL.
- **SAFE** `164, 165, 167, 168, 172, 173, 179, 181, 183, 184, 185, 186, 200` (b/c/d), Request field visits and sorting comparator. The header read, local vector sort and output assembly all run synchronously. The captureless comparator uses its arguments only within the sort.
- **SAFE** `205, 207, 208, 213, 214, 216, 217, 221, 222, 223, 224` (b/d), Connection token views into copied field strings. The outer range-for owns the temporary values vector for the loop's lifetime, and the inner token views are read before its current string can disappear.
- **SAFE** `230, 234, 257, 258, 263, 265, 289, 292, 293, 296, 297, 299, 300, 301` (b/d), Response head parser input and output. Reason, name and value views borrow the caller's unmodified buffer during parsing; returned fields and reason own their strings.
- **SAFE** `306, 310, 311, 313, 320, 321, 324, 327, 344, 347, 352, 374, 375, 376` (b/d), Framing token traversal and incremental chunk decode. Transfer-coding views borrow the immutable head during a synchronous scan, and the chunk decoder copies payload bytes before returning its status and offset.
- **SAFE** `411, 412, 413, 414, 417, 422, 424, 425` (b/d), Byte queue compaction and caller views. Current exchange and pull callers consume or copy views before queue mutation or native I/O, and the native append sources are separate owned read buffers.

### `src/rt/fetch_transport_internal.hpp`

Reviewed the internal transport declarations against their call sites and the Asio execution-context shutdown path.

- **SAFE** `29, 31, 33, 35, 44, 55, 56, 68, 69, 71, 73, 75, 83, 89, 98, 103, 112, 126, 127, 133, 136, 143, 146, 147, 148, 151` (b/d), Synchronous parser, codec and byte queue interfaces. Parser and codec callers use borrowed views within the current operation, and their returned protocol values and appended buffers own their storage.
- **SAFE** `160, 162, 163, 171, 177, 179, 182, 183, 188, 192, 195, 198, 202, 204, 207, 208, 211, 212, 213, 214, 215, 217` (b/c), Native connection tasks and receiver contract. Connect pins the receiver and ClientState for handshake, fill wraps its child with shared ownership, and the detached writer owns the connection and passes a separate buffer owner.
- **SAFE** `226, 228, 229, 233, 235, 236, 237, 240, 241, 242, 244, 245, 246, 252, 256, 257, 258, 261, 262` (b/c/d), Pool slots and Asio service ownership. The context owns the registry, whose map holds the pools. Slot references are synchronous, the timer captures a weak pool, and registry shutdown releases map owners before service destruction.
- **SAFE** `267, 268, 269, 272, 274, 275, 276, 278, 281, 285, 287, 288, 292` (b/c/d), Client per-loop registry observers and send inputs. The client uses the registry pointer only while a pool on that loop can be promoted. Current strong pool copies stay within synchronous code or a coroutine handled by the same context.

### `src/rt/fetch_transport_net.cpp`

Reviewed the TCP and TLS paths, cancellation, idle timers, pool slots, registry teardown and both TLS context ownership modes.

- **SAFE** `40, 41, 43, 44, 45, 48, 49, 52, 53, 56, 57, 58, 59, 60, 61, 64, 67, 70, 71, 72, 75, 76` (b/c), Connection idle timer and close. The registered timer holds a weak connection, promotes it only for synchronous close, and is disarmed by close or destruction.
- **SAFE** `79, 81, 83, 84, 87, 89, 90, 94, 95, 99, 109, 110, 111, 120, 121, 124, 126, 131, 132, 133, 136` (b/c/d), Handshake receiver, TLS context and read buffer. Connect owns the connection and client until handshake finishes, SSL takes the two BIOs, and the peer certificate reference is released synchronously after checking identity.
- **SAFE** `141, 143, 146, 147, 150, 151, 152, 156, 162, 163, 165, 175, 177, 178, 179, 182, 183` (b/c/d), Plain and TLS fill buffer owners. The fill wrapper pins its receiver even when cancellation clears the pump slot. Only the active response reader mutates the receiver's read buffer and input queue.
- **SAFE** `187, 188, 190, 192, 193, 195, 198, 200, 201, 207, 208, 214, 215, 217, 219, 220, 223, 224, 226` (b/d), Native write buffers through socket completion. The uploader owns the connection and the write frame owns its byte-buffer token. TLS flush copies ciphertext into its own vector before each write.
- **DEFECT (A-FETCH-NET-GCC-AWAIT)** `111, 151, 178, 193, 224` (e), Inline Asio buffer construction in native awaits. The awaits embed a constructed buffer descriptor. The port rule requires naming it before the await; current Clang compilation and the descriptor's trivial destruction do not demonstrate a GCC 12 failure.
- **SAFE** `231, 233, 235, 236, 240, 241, 243, 251, 252, 253, 260, 262, 271, 273, 275, 276, 277, 278, 281, 282, 288, 290, 292, 295, 296, 299` (b/d), Idle connection probe and slot handoff. Pool scanning stays synchronous, moves a connection into a local owner before checking it, and only returns idle sockets whose upload and parser phase has ended.
- **SAFE** `302, 304, 305, 309, 310, 311, 313, 314, 317, 323, 325, 326, 331, 335, 336, 338, 340, 341, 342, 345, 348, 350, 351, 353, 354, 358` (c/d), Pool timer reschedule and close traversal. The loop owns the timer job, the job temporarily promotes a weak pool, and no vector slot is borrowed across suspension or user callback invocation.
- **SAFE** `361, 363, 364, 365, 369, 371, 372, 373, 374, 379, 384, 387, 388, 392, 393, 394, 395, 396, 399, 402, 403, 405, 406, 408, 409, 410, 414, 415, 417, 418, 419, 420, 425, 427` (b/c/d), Registry and TLS context lifetime during client teardown. Asio releases registry pool owners during service shutdown. The only other current pool owners are synchronous handles or same-context tasks destroyed before service deallocation, and the client and SSL connections hold their own context references.

### `src/rt/gzip.cpp`

Reviewed the native and dynamically loaded zlib paths, incremental borrowed buffers, synchronous visitor delivery and the two Task wrappers.

- **SAFE** `77, 88, 89, 92, 93, 97, 98, 102, 103, 105, 108, 109, 115, 117, 121, 126, 127, 134, 135, 139, 140, 141, 451, 452, 453` (b/c/d), Zlib callback table across test seam replacement. An existing inflater holds a pointer to a static or list-owned table. Repointing the lookup slot preserves the old list node and its open library mapping.
- **SAFE** `148, 150, 152, 153, 156, 158, 160, 161, 162, 165, 166, 179, 182, 185, 190, 192, 195, 197, 198, 202, 203, 205` (b/c/d), Allocator callback storage and output spans. Zlib registers stateless allocation functions and each allocated block owns its size header. Output spans are consumed synchronously before the next growth or transfer.
- **SAFE** `216, 217, 219, 220, 229, 236, 239, 240, 244, 248, 253, 254, 259, 263, 272, 275, 283, 284, 291, 292, 294, 297, 300, 302, 331, 333, 337, 339, 340, 341, 349, 350, 351, 353, 355, 356, 362, 365, 367, 370, 372, 375, 379, 380` (b/c/d), Incremental inflater state and documented span borrows. The nonmovable heap state stays stable when the public handle moves. Checked callers keep input alive until drained, consume the current output span before another operation and stop on error.
- **SAFE** `386, 387, 389, 390, 391, 395, 396, 399, 401, 406, 407, 410, 411, 415, 418, 419, 422, 424, 426, 429, 431, 432` (b/c/d), Synchronous compressor cleanup and piece visitor. The scope guard runs with its stream and static API still alive. The visitor runs synchronously, and the production and fixture callbacks copy each piece before the next inflater read.
- **SAFE** `437, 438, 439, 440, 441, 444, 445, 446, 447, 448` (a/b/c/e), Compression and decompression Task frames. Each frame owns the input and result. Its noncoroutine scrub guard borrows the frame's input, and only the captureless zero-argument microtask runs after the codec work.

### `src/rt/tls.cpp`

Read the trust-store setup, native ASN.1 ownership, hostname matching and C API callers. These routines do not suspend.

- **SAFE** `21, 22, 24, 27, 28, 31, 32, 33, 36, 41, 44, 48, 51, 56, 57, 61, 65, 67, 69, 76, 79, 80, 81, 86, 87, 90` (b/d), File, ASN.1 and certificate-name views. The file stays open for the iterator copy, the certificate owner outlives synchronous extraction, and hostname matching reads unchanged local string vectors.
- **SAFE** `94, 95, 96, 97, 98, 99, 102, 105, 106, 108, 109, 112, 113, 119, 120, 121, 122, 123, 125, 126, 127, 132, 133, 136, 138` (b/c), Trust store and in-memory PEM ownership. The caller's PEM string stays alive until BIO_free, the store retains its certificate references, and the default context and its trust store are process-owned.
- **SAFE** `142, 171, 172, 173, 179, 180, 181, 182, 188, 189, 192, 198, 202, 203, 204, 207, 208, 210, 211, 213, 215, 219, 221, 224, 225, 226, 231, 235, 239, 241, 242, 246` (b/d), OpenSSL name extraction and result copies. The names list and caller-owned certificate support the synchronous reads, while DNS names, IP addresses, error text and the returned reason are copied into owning strings or vectors.

### `src/rt/tls_internal.hpp`

Checked the shared TLS declarations against Fetch and WebSocket callers in both context modes.

- **SAFE** `19, 23, 25, 28, 30, 34` (b/c), TLS helper API and ownership handoff. The context accessors document whether callers receive ownership. The remaining declarations borrow their inputs only synchronously and return owned result values.

### `src/rt/websocket.cpp`

Reviewed live and teardown ownership for client and server sockets, synchronous emitter reentrancy, and the native gateway upgrade caller. This file contains no coroutine bodies.

- **SAFE** `25, 31, 32, 36, 38, 40, 43, 44, 50, 51, 54, 59, 61, 62, 63, 448, 449` (c/d), Weak registry and listener teardown. The Asio service tracks weak identities. Shutdown moves the registry before promotion and pins each WebSocket locally while clearing listener cycles.
- **DEFECT (A-WEBSOCKET-CONTEXT)** `35, 42, 44, 47, 49, 50, 51, 480, 481, 482` (b/c), A retained WebSocket outlives its Asio socket service. The service closes native handles and clears callbacks during shutdown, while the public WebSocket and its Sender still own the Asio socket object.
- **SAFE** `75, 77, 79, 80, 138, 140, 146, 150, 154, 156, 158, 167, 168, 240, 242, 245, 251, 643, 647, 651, 654, 656, 657, 659, 660, 662, 669, 678, 679, 713, 722, 723, 732, 738` (b/d), HTTP handshake and byte or string views. Header lookup and protocol parsing consume borrowed storage synchronously and copy it into owning results before an upgrade or deferred operation.
- **SAFE** `172, 173, 177, 178, 182, 183, 185, 187, 193, 198, 202, 203, 207, 208, 209, 215, 217, 218, 219, 221` (c/d), Listener identity under mutation during delivery. The snapshot retains callbacks and fired flags while once removal and ordinary registration mutations affect only the live registry. Native delivery pins the receiver separately.
- **DEFECT (A-WEBSOCKET-EMIT-OWNER)** `206, 208, 215, 219, 221, 586, 587, 588, 589, 607, 608, 612` (c/d), Public event delivery can lose its WebSocket receiver. Direct emit through the public EventEmitter API has no owner to pin the derived WebSocket throughout snapshot iteration.
- **SAFE** `66, 68, 73, 233, 234, 258, 259, 265, 266, 267, 276, 277, 287, 288, 296, 297, 301, 303, 305, 306` (b/c), Synchronous dispatch adapters and deferred post-close send result. Native event dispatch is reached through an owning WebSocket handle, stack event arguments stay local, and the posted send result captures its callback and reason by value.
- **SAFE** `271, 272, 273, 309, 310, 312, 314, 315, 316, 319, 323, 325, 326, 327, 330, 332, 334, 389, 391, 396, 399, 400, 403, 409, 410` (c), Close timer and receiver finish ownership. The close timer owns its native socket, and close detaches the timer and drains the receiver finish callback. Final delivery or registry teardown clears the remaining listener owners.
- **SAFE** `339, 342, 343, 344, 345, 347, 348, 351, 354, 362, 365, 372, 373, 375, 376, 377, 378, 379, 381, 382, 384, 386` (c/d), Receiver and transport callback graph. Receiver hooks promote weak WebSocket links. Native transport hooks keep the public socket alive until close or loop teardown, and each dispatched onData consumes its bytes while the handler owns the buffer.
- **SAFE** `413, 418, 419, 420, 421, 424, 427, 428, 429, 430, 431, 434, 436, 437, 452, 453, 454, 456, 458, 459, 467, 468` (b/c), Handshake abort and construction ownership. The connecting caller pins the WebSocket, abort clears request registrations synchronously, and pending close/error notifications own the WebSocket through dispatch.
- **UNCLEAR** `473, 474, 475, 478` (c), Injected buffered-amount getter replaces itself. The production getter is stable. The supported test override would need an explicit receiver/callable ownership rule or confirmation for reentrant self-replacement.
- **SAFE** `489, 492, 497, 500, 504, 508, 512, 513, 517, 519, 521, 523, 526, 529, 533, 536, 539, 543, 546, 550, 555` (b/c), Public writes and close-frame completion. The Sender materializes write bytes synchronously. Close's completion promotes a weak socket before accessing state, and native close is scheduled with an owning Socket handler.
- **SAFE** `565, 566, 568, 570, 573, 574, 577, 578, 579, 586, 587, 588, 590, 595, 596, 597, 601, 606, 607, 608, 612, 614, 619, 620, 621, 626, 631, 632, 633, 634` (c/d), Attribute wrapper capture and replacement in native dispatch. The listener snapshot owns the callable while the native dispatch retains its WebSocket target. Direct public emit has the separate receiver pinning issue.
- **SAFE** `684, 685, 688, 690, 692, 693, 694, 696, 697, 704, 709, 710` (b/c), Rejected upgrade response ownership. The native Socket takes the moved stream and owns pending writes, while the finish hook uses a weak backedge and the response bytes leave their local string before dispatch.
- **DEFECT (A-WEBSOCKET-UPGRADE-OWNER)** `741, 750, 752, 759, 762, 768, 769, 770, 771, 781` (c), Upgrade hooks can release the last WebSocketServer owner. The server method calls external factory, protocol and headers hooks before reusing its own options and client list. The gateway's named upgradeServer retains it, while the public method borrows its receiver.
- **SAFE** `750, 752, 759, 760, 761, 762, 763, 765, 768, 769, 770, 771, 772, 773, 774, 775, 776, 777, 778, 781, 784, 786, 791, 792, 794, 795, 798, 801, 805, 808` (b/c/d), Upgrade inputs and tracked-client close. The gateway owns request and socket inputs through synchronous delivery. The tracking callback uses weak links, erases a client before posting the retained server's close notification, and leaves no client iterator suspended.

### `src/rt/websocket_client.cpp`

Reviewed Unix and TCP resolve/connect paths, TLS callbacks, HTTP response parsing, and the opening handshake's success, failure, cancellation and unexpected-response branches. This translation unit has no coroutine bodies.

- **SAFE** `59, 61, 66, 67, 71, 73, 77, 78, 97, 107, 109, 113, 115, 117, 121, 126, 128, 131, 140, 142, 152, 153, 156, 158, 162, 169, 170` (b/d), Synchronous header and response parser borrows. All input views and container iterators remain inside the parser or builder. ParsedResponse owns the copied header values passed to callbacks.
- **SAFE** `177, 179, 181, 183, 184, 185, 186, 188, 189, 194, 195, 196` (c), Opening timeout registration. The timer hook promotes a weak request and copies the current listener for its call. The request clears its timer on abort, error and completion.
- **SAFE** `197, 198, 199, 201, 203, 204, 205, 206, 207, 208, 209` (b/c), Unix-domain connect and cancellation. The Asio completion owns the request and Unix socket, and the independent cancellation hook owns the same socket until completion or abort clears it.
- **SAFE** `214, 215, 216, 217, 219, 220, 225, 227, 229, 233, 234, 237, 238, 244, 246, 249, 250, 251, 253, 255, 259, 266, 267, 268` (b/c/d), TCP resolve, retry and socket handoff. Each operation holds the request and its socket or resolver, endpoints are copied into its completion, and the retry callable has a weak self-edge cleared on terminal completion.
- **SAFE** `273, 274, 275, 276, 277, 278, 281, 282, 284, 285, 287, 288, 289, 292, 293, 295, 296, 303, 304, 305, 306, 308, 309, 312, 313, 314` (b/c), Connected socket and TLS callback ownership. The request owns its socket and each hook promotes a weak request for the synchronous call. The TLS handoff and socket write transfer values before their callbacks can run.
- **SAFE** `321, 323, 326, 332, 333, 334, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 361, 364, 365, 366, 367, 368, 371, 373, 374, 375, 376, 378, 381, 382, 383, 384, 385` (b/c/d), Response upgrade, failure and abort cleanup. The parser copies any surplus bytes, then copies the active callback before clearing the registration. Abort moves the cancellation callable out before calling it.
- **UNCLEAR** `284, 289, 351, 353, 354, 357, 358, 492, 498, 499, 503` (c), Unfinished ownership after a handled unexpected response. A registered unexpected-response listener takes the path that leaves request listeners installed until the connection closes. The listener contract needs to establish who must close a persistent response, including a listener that throws or retains no client handle.
- **SAFE** `390, 391, 395, 399, 412, 414, 421, 422, 423, 429, 431, 443, 444, 449, 451, 464, 467, 473, 475, 476, 477, 481, 482, 483, 486, 488, 489, 490, 492, 493, 495, 498, 505, 507, 508, 511, 512, 513, 515, 517, 521, 523` (b/c/d), Client initialization and handshake listener arguments. Request construction copies local options and headers. Handshake callbacks own response, socket and key values, and their public WebSocket backedge is released on normal abort, close, error or upgrade, or registry shutdown.

### `src/rt/websocket_frame.cpp`

Reviewed synchronous receiver parsing, event delivery, receiver error and finish notifications, and all Sender frame construction. There are no coroutine bodies in this translation unit.

- **SAFE** `23, 24, 28, 29, 30, 38, 44, 45, 48, 51, 52, 55, 56, 57, 61, 89, 93, 109, 125, 126, 135, 136, 157, 158, 169, 170, 171, 173, 174` (b/d), Receiver input and parser views. Socket supplies bytes to a synchronous copy, and each parse view is consumed before deferred error reporting or dispatch of a callback that could change the receiver.
- **SAFE** `178, 183, 186, 189, 191, 193, 197, 198, 199, 202, 207, 212, 213, 214, 215, 218, 219, 220` (b/c/d), Completed message and control-frame delivery. The parser moves fragment storage out before calling listeners, passes owned event bytes and invokes an owning callable copy while the native dispatch retains the receiver.
- **SAFE** `223, 226, 228, 229, 230, 231, 232, 233, 237, 240, 241, 242, 243, 247, 248, 249, 250, 251, 252, 253` (c), Receiver error and finish microtasks. The posted callbacks retain the receiver and copy each hook before invoking it. Close and registry shutdown clear the receiver's stored hooks.
- **SAFE** `258, 260, 262, 264, 265, 266, 268, 269, 270, 274, 276, 279, 286, 289, 291, 294, 297, 298, 300, 303, 304, 306, 309, 310, 311, 313, 314, 315` (b/d), Sender frame assembly and data borrows. The frame serializes into owned bytes before its stack header and caller data can expire. Fragment bookkeeping is synchronous and precedes the socket write.
- **SAFE** `258, 294, 300, 306, 315, 318` (b/c), Sender-to-socket ownership handoff. Sender holds the socket, and write receives owning frame bytes and a moved callback. The native write buffer lifetime is assessed in websocket_socket.cpp.

### `src/rt/websocket_internal.hpp`

Reviewed internal callback declarations, ByteView use, TLS inputs and all shared ownership arcs in WebSocket state. Definitions carry the coroutine-free callback and native operation analysis.

- **SAFE** `21, 24, 29, 31, 33, 37, 40, 43, 48, 50` (b/c), Synchronous event guard and custom microtask. The guard invokes its borrowed callback immediately. nextTick instead moves an owning callback to the loop and returned headers or errors own their values.
- **SAFE** `57, 59, 60, 61, 65, 66, 67, 68, 69, 71, 75, 79, 83, 87, 90, 92, 109, 123, 124, 126, 128, 131, 132, 136, 138, 139, 140, 155` (b/c), Native Socket buffer and callback declarations. The socket owns read storage and makes delivery spans call-scoped. Posted read and event callbacks capture strong owners, and the implementation distinguishes write-queue ownership during cancellation.
- **SAFE** `159, 170, 172, 173, 174, 175, 176, 180, 182, 185, 189, 198, 199, 203, 212, 225, 227, 230, 231, 232, 233, 238, 239` (b/c/d), Receiver and Sender byte and frame interfaces. The receiver copies payloads before emitting and uses short views only during parsing. Sender copies source bytes to an owning frame before the socket write and holds its socket by shared owner.
- **SAFE** `246, 256, 257, 259, 260, 261, 262, 265, 267, 271, 274, 277, 279, 282, 283, 285, 286, 290, 300, 304, 305, 306, 307, 308, 310` (b/c), Handshake and cross-file handoff declarations. Connection targets, upgrade bytes and operation handles are retained by the appropriate callback or local caller; response parser views are consumed synchronously.
- **DEFECT (A-WEBSOCKET-WRITE-BUFFER)** `114, 115, 121, 127, 128, 141, 145` (b/c), The pending queue is the sole asynchronous write-buffer owner. Its stored data can disappear when shutdown discards the write queue. The handler needs ownership independent of queue membership.
- **DEFECT (A-WEBSOCKET-CONTEXT)** `314, 315, 324, 325, 326, 335` (b/c), WebSocket state retains loop-bound socket members. The WebSocket owns both Sender and Socket across context shutdown if an external owner retains the public handle.

### `src/rt/websocket_socket.cpp`

Reviewed plaintext and TLS byte storage, synchronous callback reentrancy, Asio initiation and completion, ordinary close, cancellation, and context shutdown. There are no coroutine bodies, but Asio completions suspend access to the native buffers.

- **SAFE** `60, 61, 66, 71, 73, 78, 82, 84, 85, 86, 87, 88, 90, 96, 99, 101, 102, 105, 106, 111, 112, 113, 117, 118, 142, 145, 147, 151, 153, 156, 157` (b/c/d), Read buffer and held-input delivery. The outstanding Asio read owns Socket and its stable readBuffer_. Deferred held-input delivery removes each entry before dispatch, while native and TLS spans are consumed synchronously.
- **SAFE** `119, 120, 121, 122, 123, 124, 127, 131, 134, 137, 160, 163, 164, 166, 167` (c), EOF and read-error callbacks. The read handler pins Socket and moves or copies the active callable before invocation. EOF handling checks destroyed_ after emitting end.
- **DEFECT (A-WEBSOCKET-WRITE-BUFFER)** `188, 190, 194, 196, 199, 201, 202, 203, 226, 276, 280, 281, 284, 305, 308, 309` (b/c/d), Closing a WebSocket releases the native write buffer before completion. The queued bytes are the sole buffer owner, and the handler owns Socket without independently pinning the pending entry. Asio requires the storage through completion even when close cancels the operation.
- **SAFE** `140, 172, 175, 179, 182, 186, 210, 211, 214, 215, 216, 219, 224, 225, 232, 233, 236, 237, 238, 239, 240, 242` (c/d), Write progress, activity and user callbacks. The installed activity handler only refreshes the current handshake timer. User completions run after their queue entry is removed, and onWritten rechecks whether they destroyed Socket.
- **SAFE** `245, 248, 251, 256, 258, 259, 265, 267, 268, 272, 273, 281, 284, 285, 288, 289, 291, 294, 297, 299, 301, 302, 305, 309, 311, 312, 314, 317, 318, 319, 320, 321, 322, 323` (c/d), End, destroy and listener release. Scheduled end and close operations own Socket, pending completions move out for one-shot delivery, and close/error use local listener copies before removing stored hooks.
- **SAFE** `42, 48, 49, 50, 55, 56, 328, 331, 332, 337, 338, 339, 341, 342, 345, 347, 348, 349, 350, 351, 352, 355, 361, 362, 363, 375, 376, 378, 379, 384, 385, 386, 387, 394, 395` (b/c), TLS setup, certificate views and handshake completion. OpenSSL receives synchronously consumed option storage and owns its BIOs after setup, while callbacks own Tls options and the native receiver is pinned by the handshake caller or read handler.
- **SAFE** `390, 391, 392, 393, 398, 399, 400, 401, 404, 405, 407, 411, 413, 414, 416, 419, 421, 424, 426, 428, 429, 444, 445, 448, 450, 451, 452, 453, 455` (b/c/d), TLS plaintext and ciphertext buffers. The handshake moves the pending plaintext queue out before flushing it, TLS views stay in a synchronous delivery, and owned ciphertext moves into the native write queue.

## Ranked defects and counts

Reviewed **30/30 files** and **180 site groups**. A site group can have multiple line anchors and hazard classes under the same verdict.

| Hazard class                                | DEFECT | SAFE | UNCLEAR |
| ------------------------------------------- | -----: | ---: | ------: |
| a. Capturing coroutine closure              |      5 |   20 |       0 |
| b. Borrowed coroutine parameter or receiver |     15 |  113 |       3 |
| c. Callback ownership or cycle              |     22 |  111 |       6 |
| d. Container invalidation                   |      5 |   76 |       0 |
| e. GCC 12 await-expression rule             |      5 |   13 |       1 |
| **Unique site groups**                      |     29 |  144 |       7 |

### 1. A-WEBSOCKET-WRITE-BUFFER (native write-buffer lifetime on WebSocket request paths) — Closing a WebSocket releases the native write buffer before completion

`src/rt/websocket_socket.cpp:188, 190, 194, 196, 199, 201, 202, 203, 226, 276, 280, 281, 284, 305, 308, 309`

- A send reaches would-block and async_write_some borrows the front item's bytes. Its completion has yet to run.
- A normal terminate or loop teardown closes the descriptor and discards the queue before the completion is delivered or destroyed.

```diff
--- a/cpp/gateway/src/rt/websocket_internal.hpp
+++ b/cpp/gateway/src/rt/websocket_internal.hpp
@@ -114,3 +114,3 @@
     struct Pending {
-      Bytes data;
+      std::shared_ptr<const Bytes> data;
       std::size_t offset = 0;
--- a/cpp/gateway/src/rt/websocket_socket.cpp
+++ b/cpp/gateway/src/rt/websocket_socket.cpp
@@ -188,4 +188,4 @@
   void Socket::queueRaw(Bytes data, std::size_t accounted, Callback callback) {
     queued_ += accounted;
-    queue_.push_back(Pending{std::move(data), 0, accounted, std::move(callback)});
+    queue_.push_back(Pending{std::make_shared<const Bytes>(std::move(data)), 0, accounted, std::move(callback)});
     if (!writing_) flush();
@@ -196,9 +196,13 @@
       Pending& front = queue_.front();
-      if (front.offset < front.data.size()) {
+      if (front.offset < front.data->size()) {
+        const auto buffer = asio::buffer(front.data->data() + front.offset, front.data->size() - front.offset);
         boost::system::error_code error;
-        const std::size_t written = stream_.write_some(asio::buffer(front.data.data() + front.offset, front.data.size() - front.offset), error);
+        const std::size_t written = stream_.write_some(buffer, error);
         if (error == asio::error::would_block || error == asio::error::try_again) {
           writing_ = true;
-          stream_.async_write_some(asio::buffer(front.data.data() + front.offset, front.data.size() - front.offset),
-                                   [self = shared_from_this()](const boost::system::error_code& failure, std::size_t count) { guarded([&] { self->onWritten(failure, count); }); });
+          stream_.async_write_some(buffer, [self = shared_from_this(), data = front.data](const boost::system::error_code& failure, std::size_t count) {
+            (void)data;
+            guarded([&] { self->onWritten(failure, count); });
+          });
           return;
@@ -210,3 +214,3 @@
         front.offset += written;
         if (auto activity = onActivity) activity();
-        if (front.offset < front.data.size()) continue;
+        if (front.offset < front.data->size()) continue;
@@ -233,3 +237,3 @@
     Pending& front = queue_.front();
     front.offset += count;
-    if (front.offset == front.data.size()) {
+    if (front.offset == front.data->size()) {
```

### 2. A-FETCH-ABORT-TRANSPORT (memory safety in the injectable transport path) — Abort can outlive the owning virtual transport frame

`src/rt/fetch.cpp:131, 132, 134, 135, 136, 137, 138, 195`

- An installed transport's member send coroutine suspends on an rt Promise. Replace the global transport so sendThrough is its remaining owner, then abort the signal.
- abortable rejects sendThrough immediately but cannot interrupt the rt Promise. sendThrough releases the transport while the child's member frame remains pending.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -136,2 +136,3 @@
       Task<Response> sending = transport->send(std::move(request));
-      Task<Response> guarded = abortable(signal, std::move(sending));
+      auto ownedSend = detail::keepAlive<Response>(std::move(transport), std::move(sending));
+      Task<Response> guarded = abortable(signal, std::move(ownedSend));
```

### 3. A-STREAM-CANCEL-OWNER (memory safety in stream cancellation) — Stream cancel borrows the owner slot through callback invocation

`include/uos/rt/stream.hpp:238, 249, 251, 252, 253, 262, 264, 336, 338, 352, 355, 396, 398, 401, 404`

- reader.cancel closes the stream and invokes the underlying cancel callback while borrowing reader->stream.
- The callback releases the reader lock. The same shared_ptr slot becomes null, then cancelStreamInto dereferences it to clear the algorithms.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -238,1 +238,1 @@
-    template <class T> void cancelStreamInto(const std::shared_ptr<StreamState<T>>& stream, Reason reason, const std::shared_ptr<PromiseState<void>>& out) {
+    template <class T> void cancelStreamInto(std::shared_ptr<StreamState<T>> stream, Reason reason, const std::shared_ptr<PromiseState<void>>& out) {
```

### 4. A-FETCH-CALLBACK-INPUT (memory safety in the injectable Fetch API) — Fetch erasure can discard input storage before a borrowing coroutine starts

`include/uos/rt/fetch.hpp:20, 27, 28, 29, 33, 38, 40, 41, 49`

- Construct Fetch with a coroutine callable taking const RequestInfo& or const RequestInit&. Fetch invokes the by-value std::function and returns a lazy Task.
- The full expression in Fetch::operator() ends and destroys the erased argument. The child resumes with a dangling reference even though keepAlive retains the callable.

```diff
diff --git a/cpp/gateway/include/uos/rt/fetch.hpp b/cpp/gateway/include/uos/rt/fetch.hpp
--- a/cpp/gateway/include/uos/rt/fetch.hpp
+++ b/cpp/gateway/include/uos/rt/fetch.hpp
@@ -20 +20 @@
-    using Function = std::function<Task<Response>(RequestInfo input, RequestInit init)>;
+    using Function = std::function<Task<Response>(RequestInfo&& input, RequestInit&& init)>;
@@ -49 +49,2 @@
-    static Task<Response> keepAlive(std::shared_ptr<const Function> function, Task<Response> task);
+    using Inputs = std::pair<RequestInfo, RequestInit>;
+    static Task<Response> keepAlive(std::shared_ptr<const Function> function, std::shared_ptr<Inputs> inputs, Task<Response> task);
diff --git a/cpp/gateway/src/rt/fetch.cpp b/cpp/gateway/src/rt/fetch.cpp
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -176,4 +176,7 @@
   Task<Response> Fetch::operator()(RequestInfo input, RequestInit init) const {
-    if (!function_) throw std::logic_error("rt::Fetch: called an empty fetch value");
-    return keepAlive(function_, (*function_)(std::move(input), std::move(init)));
+    auto function = function_;
+    if (!function) throw std::logic_error("rt::Fetch: called an empty fetch value");
+    auto inputs = std::make_shared<Inputs>(std::move(input), std::move(init));
+    auto task = (*function)(std::move(inputs->first), std::move(inputs->second));
+    return keepAlive(std::move(function), std::move(inputs), std::move(task));
   }
@@ -181,3 +184,4 @@
-  Task<Response> Fetch::keepAlive(std::shared_ptr<const Function> function, Task<Response> task) {
+  Task<Response> Fetch::keepAlive(std::shared_ptr<const Function> function, std::shared_ptr<Inputs> inputs, Task<Response> task) {
     (void)function;
+    (void)inputs;
     Response response = co_await std::move(task);
```

### 5. A-FETCH-REENTRANT-FUNCTION (memory safety during a reentrant Fetch callback) — Fetch calls a replaceable slot before reliably taking ownership

`src/rt/fetch.cpp:176, 177, 178, 181, 183, 189, 191, 193`

- fetch invokes a synchronous Fetch callback through the global slot. The callable argument to keepAlive is evaluated first and the callback replaces globalThis.fetch.
- Replacement drops the callback's only owner. Its remaining code or returned child can borrow the destroyed closure, and keepAlive can pick up the replacement.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -177,2 +177,4 @@
-    if (!function_) throw std::logic_error("rt::Fetch: called an empty fetch value");
-    return keepAlive(function_, (*function_)(std::move(input), std::move(init)));
+    auto function = function_;
+    if (!function) throw std::logic_error("rt::Fetch: called an empty fetch value");
+    auto task = (*function)(std::move(input), std::move(init));
+    return keepAlive(std::move(function), std::move(task));
```

### 6. A-MOCK-HANDLER-OWNER (memory safety in local transport injection) — Mock transport returns a callback task without its handler owner

`src/rt/fetch.cpp:204, 205, 206`

- Call MockTransport::send with a capturing coroutine handler and release the mock while retaining the returned task.
- The lazy handler then runs with a dangling closure. A handler borrowing the erased Request also loses that argument at the end of send's call expression.

```diff
--- a/cpp/gateway/include/uos/rt/fetch.hpp
+++ b/cpp/gateway/include/uos/rt/fetch.hpp
@@ -105,2 +105,2 @@
-    using Handler = std::function<Task<Response>(Request request)>;
-    explicit MockTransport(Handler handler) : handler_(std::move(handler)) {}
+    using Handler = std::function<Task<Response>(Request&& request)>;
+    explicit MockTransport(Handler handler) : handler_(std::make_shared<const Handler>(std::move(handler))) {}
@@ -112 +112 @@
-    Handler handler_;
+    std::shared_ptr<const Handler> handler_;
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -205,2 +205,4 @@
     requests_.push_back(request);
-    return handler_(std::move(request));
+    auto invocation = std::make_shared<std::pair<std::shared_ptr<const Handler>, Request>>(handler_, std::move(request));
+    auto task = (*invocation->first)(std::move(invocation->second));
+    return detail::keepAlive<Response>(std::move(invocation), std::move(task));
```

### 7. A-PROMISE-CALLBACK-INPUT (memory safety in the public runtime API) — Task-returning Promise callbacks can borrow a destroyed outcome

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

### 8. A-ABORT-OWNER (memory safety in the public runtime API) — Abort dispatch borrows the controller's state slot

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

### 9. A-FETCH-ABORT-BODY-CYCLE (request-path leak) — Dropped abortable response body is kept by its own controller

`src/rt/fetch.cpp:70, 72, 73, 74, 77, 112, 114, 115, 120, 123, 124, 125, 126, 127, 128`

- A request with an explicit signal receives a streaming response. The front end caches the wrapper's controller in the owner retained by its pull and cancel callbacks.
- The caller drops the response without reading it or aborting. No pull occurs, and the controller, stream algorithms, source reader and signal remain retained.

```diff
--- a/cpp/gateway/src/rt/fetch.cpp
+++ b/cpp/gateway/src/rt/fetch.cpp
@@ -72 +72 @@
-      std::optional<ReadableStreamDefaultController<ByteChunk>> controller;
+      std::weak_ptr<detail::StreamState<ByteChunk>> wrapper;
@@ -76,2 +76,3 @@
       explicit AbortableBody(AbortSignal signal_) : signal(std::move(signal_)) {}
+      ~AbortableBody() { finish(); }
       void finish() {
@@ -120,2 +121,2 @@
-        if (held->controller) held->controller->error(reason);
+        if (auto wrapper = held->wrapper.lock()) wrapper->error(reason);
         spawn(held->reader->cancel(reason), [](Reason) {});
@@ -124,5 +125,6 @@
-      wrapper.start = [body](ReadableStreamDefaultController<ByteChunk> controller) { body->controller = controller; };
       wrapper.pull = [body](ReadableStreamDefaultController<ByteChunk> controller) { return pullAbortable(body, controller); };
       wrapper.cancel = [body](Reason reason) { return cancelAbortable(body, std::move(reason)); };
+      ByteStream stream(std::move(wrapper), QueuingStrategy{0});
+      body->wrapper = stream.state();
       signal.addEventListener("abort", body->onAbort, AddEventListenerOptions{.once = true});
-      response.setBodyStream(ByteStream(std::move(wrapper), QueuingStrategy{0}));
+      response.setBodyStream(std::move(stream));
```

### 10. A-FETCH-UPLOAD-STRANDED (request-path retention after a failed or early response) — Terminal connection close can strand a detached upload reader

`src/rt/fetch_transport.cpp:173, 182, 203, 204, 209, 210, 215, 216, 274, 276, 277, 279, 296, 298, 299, 349, 350, 368, 372, 378, 475, 478, 479, 530, 537, 555`

- The client sends the upload head and parks its detached writer in reader.read(). The peer closes before the response head, or the early response finishes before the upload source produces another chunk.
- The terminal exchange closes the socket with an unaborted signal. The upload Promise never settles and retains the writer, stream and closed connection on the process loop.

```diff
--- a/cpp/gateway/src/rt/fetch_transport.cpp
+++ b/cpp/gateway/src/rt/fetch_transport.cpp
@@ -173 +173,6 @@
       std::optional<ReadableStreamDefaultReader<ByteChunk>> reader;
+      void releaseReader() {
+        auto active = std::exchange(reader, std::nullopt);
+        if (active) active->releaseLock();
+      }
@@ -276,2 +281,3 @@
         connection->close();
         if (writer->reader && signal.aborted()) spawn(writer->reader->cancel(signal.reason()), ignoreRejection);
+        writer->releaseReader();
@@ -377,2 +383,3 @@
       }
       connection->close();
+      pump.writer->releaseReader();
```

### 11. A-FETCH-RESPONSE-STRANDED (request-path pending response read) — Post-head upload failure leaves a response read parked

`src/rt/fetch_transport.cpp:135, 214, 215, 216, 236, 237, 238, 416, 436, 437, 438, 440, 441, 452, 455, 474`

- The response head arrives while an upload and a response body read are both pending. The upload reader rejects and writeRequest records the failure and closes the connection.
- The body fill reports aborted with pump.cancelled false. pullBody returns without closing or erroring the stream, leaving the downstream reader Promise and its continuation parked.

```diff
--- a/cpp/gateway/src/rt/fetch_transport.cpp
+++ b/cpp/gateway/src/rt/fetch_transport.cpp
@@ -439,4 +439,5 @@
           if (status == ReadStatus::data) continue;
           if (pump->connection->timedOut()) rethrow(timeoutErrorReason());
-          if (status == ReadStatus::aborted) co_return;
+          if (pump->writer->bodyFailure) rethrow(pump->writer->bodyFailure);
+          if (status == ReadStatus::aborted) throw closedError(pump->url);
           if (status == ReadStatus::eof && pump->framing == Framing::close) {
```

### 12. A-PROCESS-SIGNAL-CONTEXT (memory safety with scoped runtime loops) — Static process signal registry retains a scoped Asio context

`src/rt/process_loop.cpp:100, 102, 107, 108, 119, 124, 125, 134, 136, 142, 148`

- Register process::on inside a local LoopScope. The static signal registry constructs its Asio signal_set on that loop.
- Destroy the loop before explicitly removing the listener. Later registry use or destruction still uses the old Asio service and executor.

```diff
--- a/cpp/gateway/src/rt/process_loop.cpp
+++ b/cpp/gateway/src/rt/process_loop.cpp
@@ -106,5 +106,13 @@
+    struct SignalRegistry final : asio::execution_context::service {
+      inline static asio::execution_context::id id;
+      Signals state;
+      explicit SignalRegistry(asio::execution_context& context) : asio::execution_context::service(context) {}
+      void shutdown() override {
+        state.set.reset();
+        state.listeners.clear();
+      }
+    };
+
     Signals& signals() {
-      static Signals state;
-      return state;
+      return asio::use_service<SignalRegistry>(rt::loop().context()).state;
     }
```

### 13. A-STREAM-TEE-READ-CYCLE (retention on cloned streaming bodies) — In-flight tee read keeps abandoned branches' source alive

`include/uos/rt/stream.hpp:444, 445, 446, 453, 454, 458, 460, 464, 469, 475, 479, 485, 486, 493, 496, 502, 554, 557, 558, 559, 561, 562, 567, 568`

- Tee starts a read of a source that has no next chunk yet, registering the continuation on the source read request.
- Drop both branch handles and the original source. The read request owns the parked frame, whose TeeState parameter owns the source and its read queue until external settlement or Loop teardown.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -460,5 +460,12 @@
-    template <class T> Task<void> teeRead(std::shared_ptr<TeeState<T>> tee) {
+    template <class T> Task<void> teeRead(std::weak_ptr<TeeState<T>> weak) {
+      Task<ReadableStreamReadResult<T>> reading;
+      {
+        auto live = weak.lock();
+        if (!live) co_return;
+        reading = readFrom(live->reader);
+      }
       ReadableStreamReadResult<T> result;
       bool failed = false;
       try {
-        result = co_await readFrom(tee->reader);
+        result = co_await std::move(reading);
@@ -467,1 +474,3 @@
       }
+      auto tee = weak.lock();
+      if (!tee) co_return;
@@ -502,1 +511,1 @@
-      start(currentLoop(), teeRead(tee));
+      start(currentLoop(), teeRead<T>(std::weak_ptr<TeeState<T>>(tee)));
```

### 14. A-PROMISE-REACTION-CYCLE (retention on long-running loops) — Pending Promise reactions self-own their source

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

### 15. A-GENERATOR-STEP-OWNER (public runtime liveness) — Pending generator step does not keep its generator alive

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

### 16. A-LOOP-TEARDOWN-OWNER (wrong-loop teardown and retention) — Scoped Loop teardown runs callbacks under the ambient loop

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

### 17. A-WEBSOCKET-EMIT-OWNER (memory safety in explicit WebSocket event delivery) — Public event delivery can lose its WebSocket receiver

`src/rt/websocket.cpp:206, 208, 215, 219, 221, 586, 587, 588, 589, 607, 608, 612`

- Invoke public emit on a shared WebSocket and let its first listener reset the last external owner. The dispatch snapshot keeps callable identities alive.
- The next once registration accesses the destroyed registry, or an attribute wrapper uses the deleted WebSocket as its target.

```diff
--- a/cpp/gateway/include/uos/rt/websocket.hpp
+++ b/cpp/gateway/include/uos/rt/websocket.hpp
@@ -165,2 +165,3 @@
   protected:
+    virtual std::shared_ptr<const void> pinEventReceiver() const { return {}; }
     std::vector<Registration> registrations_;
@@ -339,1 +340,2 @@
   private:
+    std::shared_ptr<const void> pinEventReceiver() const override { return weak_from_this().lock(); }
@@ -441,1 +443,2 @@
   private:
+    std::shared_ptr<const void> pinEventReceiver() const override { return weak_from_this().lock(); }
--- a/cpp/gateway/src/rt/websocket.cpp
+++ b/cpp/gateway/src/rt/websocket.cpp
@@ -206,2 +206,3 @@
   bool EventEmitter::emit(std::string_view event, const EventArgs& args) {
+    const auto receiver = pinEventReceiver();
     std::vector<Registration> listeners;
```

### 18. A-WEBSOCKET-UPGRADE-OWNER (memory safety in the public server upgrade API) — Upgrade hooks can release the last WebSocketServer owner

`src/rt/websocket.cpp:741, 750, 752, 759, 762, 768, 769, 770, 771, 781`

- Call handleUpgrade using the last shared Server handle. A synchronous configured hook or headers listener drops that handle before returning.
- completeUpgrade resumes and reads options_ or mutates clients_ after the server was destroyed.

```diff
--- a/cpp/gateway/src/rt/websocket.cpp
+++ b/cpp/gateway/src/rt/websocket.cpp
@@ -741,2 +741,3 @@
   void WebSocketServer::completeUpgrade(const std::string& key, const std::vector<std::string>& protocols, const UpgradeRequest& request, UpgradeSocket& socket, Bytes head, UpgradeCallback cb) {
+    const auto self = weak_from_this().lock();
     // Destroy the socket if the client has already sent a FIN packet.
```

### 19. A-PROMISE-GCC-AWAIT (GCC 12 portability rule) — Implicit shared_ptr conversion in Promise await expressions

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

### 20. A-GENERATOR-GCC-AWAIT (GCC 12 portability rule) — Forwarded generator values constructed within await expressions

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

### 21. A-STREAM-GCC-AWAIT (GCC 12 portability rule) — Stream callback and yield arguments materialized inside awaits

`include/uos/rt/stream.hpp:72, 540`

- A stream uses an owning controller, cancellation reason or chunk in a callback/yield invocation. The argument is materialized inside co_await.
- The GCC 12 rule calls for creating the Task separately. Using an rvalue-reference erased signature keeps a borrowing callee attached to runAsync's frame while performing that split.

```diff
--- a/cpp/gateway/include/uos/rt/stream.hpp
+++ b/cpp/gateway/include/uos/rt/stream.hpp
@@ -49,1 +49,1 @@
-        else async_ = std::make_shared<const std::function<Task<void>(A)>>(std::forward<F>(callable));
+        else async_ = std::make_shared<const std::function<Task<void>(A&&)>>(std::forward<F>(callable));
@@ -72,3 +72,6 @@
-      static Task<void> runAsync(std::shared_ptr<const std::function<Task<void>(A)>> function, A argument) { co_await (*function)(std::move(argument)); }
+      static Task<void> runAsync(std::shared_ptr<const std::function<Task<void>(A&&)>> function, A argument) {
+        auto handling = (*function)(std::move(argument));
+        co_await std::move(handling);
+      }
       std::shared_ptr<const std::function<void(A)>> sync_;
-      std::shared_ptr<const std::function<Task<void>(A)>> async_;
+      std::shared_ptr<const std::function<Task<void>(A&&)>> async_;
@@ -540,1 +543,2 @@
-          co_await yield(std::move(*step->value));
+          auto yielding = yield(std::move(*step->value));
+          co_await std::move(yielding);
```

### 22. A-WEBSOCKET-CONTEXT (memory safety when a socket survives a scoped Loop) — A retained WebSocket outlives its Asio socket service

`src/rt/websocket.cpp:35, 42, 44, 47, 49, 50, 51, 480, 481, 482`

- Retain an opened or already-closed client WebSocket outside a scoped Loop. During context shutdown the registry closes its descriptor, while state.socket and Sender still hold the Asio object.
- Destroy the last public WebSocket after the Loop. Its Asio socket destructor reaches the service destroyed with the old io_context.

```diff
--- a/cpp/gateway/src/rt/websocket.cpp
+++ b/cpp/gateway/src/rt/websocket.cpp
@@ -49,3 +49,10 @@
-          if (state.socket) state.socket->shutdown();
+          if (state.socket) {
+            state.socket->shutdown();
+            state.bufferedAmount = static_cast<double>(state.socket->writableLength()) + state.sender->bufferedBytes;
+          }
+          state.sender.reset();
+          state.socket.reset();
+          state.socketAttached = false;
+          state.readyState = WebSocket::CLOSED;
           if (state.receiver) state.receiver->removeAllListeners();
           websocket->removeAllListeners();
```

### 23. A-FETCH-NET-GCC-AWAIT (GCC 12 portability rule) — Inline Asio buffer construction in native awaits

`src/rt/fetch_transport_net.cpp:111, 151, 178, 193, 224`

- The handshake, read or write initiates an Asio operation with an inline buffer descriptor inside the awaited expression.
- The expression has the construction shape forbidden by the port. Separate the descriptor from the await on both connection branches.

```diff
--- a/cpp/gateway/src/rt/fetch_transport_net.cpp
+++ b/cpp/gateway/src/rt/fetch_transport_net.cpp
@@ -109,5 +109,6 @@
       boost::system::error_code error;
       auto token = asio::redirect_error(asio::use_awaitable, error);
-      const std::size_t count = co_await socket_.async_read_some(asio::buffer(readBuffer_), token);
+      auto buffer = asio::buffer(readBuffer_);
+      const std::size_t count = co_await socket_.async_read_some(buffer, token);
       if (error || count == 0) {
         const ReadStatus status = statusOf(error, open_);
@@ -149,5 +150,6 @@
       boost::system::error_code error;
       auto token = asio::redirect_error(asio::use_awaitable, error);
-      const std::size_t count = co_await socket_.async_read_some(asio::buffer(readBuffer_), token);
+      auto buffer = asio::buffer(readBuffer_);
+      const std::size_t count = co_await socket_.async_read_some(buffer, token);
       if (!open_) co_return ReadStatus::aborted;
       if (error) co_return statusOf(error, open_);
@@ -176,5 +178,6 @@
       boost::system::error_code error;
       auto token = asio::redirect_error(asio::use_awaitable, error);
-      const std::size_t count = co_await socket_.async_read_some(asio::buffer(readBuffer_), token);
+      auto buffer = asio::buffer(readBuffer_);
+      const std::size_t count = co_await socket_.async_read_some(buffer, token);
       if (!open_) co_return ReadStatus::aborted;
       if (error) co_return statusOf(error, open_);
@@ -191,5 +194,6 @@
       boost::system::error_code error;
       auto token = asio::redirect_error(asio::use_awaitable, error);
-      co_await asio::async_write(socket_, asio::buffer(data, size), token);
+      auto buffer = asio::buffer(data, size);
+      co_await asio::async_write(socket_, buffer, token);
       if (error || !open_) co_return false;
       touch();
@@ -222,5 +226,6 @@
       boost::system::error_code error;
       auto token = asio::redirect_error(asio::use_awaitable, error);
-      co_await asio::async_write(socket_, asio::buffer(cipher.data(), static_cast<std::size_t>(taken)), token);
+      auto buffer = asio::buffer(cipher.data(), static_cast<std::size_t>(taken));
+      co_await asio::async_write(socket_, buffer, token);
       if (error) co_return false;
       touch();
```
